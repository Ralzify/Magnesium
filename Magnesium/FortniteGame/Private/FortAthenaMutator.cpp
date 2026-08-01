#include "pch.h"
#include "../Public/FortAthenaMutator.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortKismetLibrary.h"
#include "../Public/FortSafeZoneIndicator.h"
#include "../Public/FortWeapon.h"
#include "../Public/LevelStreamingDynamic.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../Erbium/Public/Configuration.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr double HeistMinimumVersion = 5.40;
    constexpr double HeistEndVersionExclusive = 6.00;
    constexpr double ExitCraftDiscoveryInterval = 0.20;
    constexpr double NativeLTMVersion = 10.40;
    constexpr double NativeLTMVersionTolerance = 0.001;
    constexpr double NativeLTMInitializationGraceSeconds = 3.0;
    constexpr double NativeLTMAttemptInterval = 1.0;
    constexpr int32 NativeLTMMaxPlaylistRepublishAttempts = 3;
    constexpr int32 Native1040SetupRecoveryMaxAttempts = 3;
    constexpr double Native1040SetupRecoveryInterval = 1.0;
    constexpr double Native1040TimerObservationGrace = 1.0;
    constexpr int32 Native1040ManualSpawnMaxAttempts = 3;
    constexpr double Native1040ManualSpawnRetryCooldown = 5.0;
    constexpr double Native1040MaximumTimerHorizon = 3600.0;
    constexpr double DiscoControlPointReconcileInterval = 1.0;
    constexpr double DiscoCoincidentPointDistance = 100.0;
    constexpr double WaxPlayerDataGraceSeconds = 1.0;
    constexpr uint8 HeistExitCraftStateNone = 0;
    constexpr uint8 HeistExitCraftStateIncoming = 1;
    constexpr uint8 HeistExitCraftStateSpawned = 2;
    constexpr uint8 HeistExitCraftStateExited = 3;
    constexpr uint8 ExitCraftActorStateNone = 0;
    constexpr uint8 ExitCraftActorStateSpawned = 1;

    struct FNativeLTMPlaylistDescriptor
    {
        const wchar_t* Path;
        const wchar_t* ObjectName;
        const char* LogName;
        const char* ExpectedMutatorBaseName;
    };

    constexpr FNativeLTMPlaylistDescriptor NativeLTMPlaylists[] = {
        {
            L"/Game/Athena/Playlists/Barrier/"
                L"Playlist_Barrier_16_B_Lava."
                L"Playlist_Barrier_16_B_Lava",
            L"Playlist_Barrier_16_B_Lava",
            "Food Fight: Deep Fried",
            "FortAthenaMutator_Barrier"
        },
        {
            L"/Game/Athena/Playlists/Barrier/"
                L"Playlist_Barrier.Playlist_Barrier",
            L"Playlist_Barrier",
            "Food Fight",
            "FortAthenaMutator_Barrier"
        },
        {
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Solo.Playlist_Bling_Solo",
            L"Playlist_Bling_Solo",
            "The Getaway Solos",
            "FortAthenaMutator_Heist"
        },
        {
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Duos.Playlist_Bling_Duos",
            L"Playlist_Bling_Duos",
            "The Getaway Duos",
            "FortAthenaMutator_Heist"
        },
        {
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Squads.Playlist_Bling_Squads",
            L"Playlist_Bling_Squads",
            "The Getaway Squads",
            "FortAthenaMutator_Heist"
        },
        {
            L"/Game/Athena/Playlists/gg/"
                L"Playlist_Gg_Reverse.Playlist_Gg_Reverse",
            L"Playlist_Gg_Reverse",
            "Arsenal Solos",
            "FortAthenaMutator_GG"
        },
        {
            L"/Game/Athena/Playlists/Wax/"
                L"Playlist_Wax_Solo.Playlist_Wax_Solo",
            L"Playlist_Wax_Solo",
            "Wick's Bounty Solo",
            "FortAthenaMutator_Wax"
        },
        {
            L"/Game/Athena/Playlists/Wax/"
                L"Playlist_Wax_Duos.Playlist_Wax_Duos",
            L"Playlist_Wax_Duos",
            "Wick's Bounty Duo",
            "FortAthenaMutator_Wax"
        },
        {
            L"/Game/Athena/Playlists/Wax/"
                L"Playlist_Wax_Squads.Playlist_Wax_Squads",
            L"Playlist_Wax_Squads",
            "Wick's Bounty Squads",
            "FortAthenaMutator_Wax"
        },
        {
            L"/Game/Athena/Playlists/Wax/"
                L"Playlist_Bounty_Solo.Playlist_Bounty_Solo",
            L"Playlist_Bounty_Solo",
            "Bounty Solo",
            "FortAthenaMutator_Wax"
        },
        {
            L"/Game/Athena/Playlists/Wax/"
                L"Playlist_Bounty_Duos.Playlist_Bounty_Duos",
            L"Playlist_Bounty_Duos",
            "Bounty Duo",
            "FortAthenaMutator_Wax"
        },
        {
            L"/Game/Athena/Playlists/Wax/"
                L"Playlist_Bounty_Squads."
                L"Playlist_Bounty_Squads",
            L"Playlist_Bounty_Squads",
            "Bounty Squads",
            "FortAthenaMutator_Wax"
        },
        {
            L"/Game/Athena/Playlists/Ashton/"
                L"Playlist_Ashton_Lg.Playlist_Ashton_Lg",
            L"Playlist_Ashton_Lg",
            "Avengers: Endgame",
            "FortAthenaMutator_Ashton"
        },
        {
            L"/Game/Athena/Playlists/50v50/Disco/"
                L"Playlist_Disco_32.Playlist_Disco_32",
            L"Playlist_Disco_32",
            "Disco Domination",
            "FortAthenaMutator_Disco"
        }
    };

    struct FResolvedNativeLTMMutator
    {
        UFortGameplayModifierItemDefinition* Modifier;
        TSoftClassPtr<UClass> Definition;
        UClass* Class;
        bool bMutatesGameMode;
        bool bMutatesGameState;

        FResolvedNativeLTMMutator(
            UFortGameplayModifierItemDefinition* InModifier,
            const TSoftClassPtr<UClass>& InDefinition,
            UClass* InClass,
            bool bInMutatesGameMode,
            bool bInMutatesGameState)
            : Modifier(InModifier),
              Definition(InDefinition),
              Class(InClass),
              bMutatesGameMode(bInMutatesGameMode),
              bMutatesGameState(bInMutatesGameState)
        {
        }
    };

    struct FNativeLTMCompatibilityState
    {
        UWorld* World = nullptr;
        const UFortPlaylistAthena* Playlist = nullptr;
        const FNativeLTMPlaylistDescriptor* Descriptor = nullptr;
        double PlaylistDataLoadedTime = -1.0;
        double NextAttemptTime = 0.0;
        double NextPlaylistRepublishTime = -1.0;
        int32 PlaylistRepublishAttempts = 0;
        bool bDefinitionsPrepared = false;
        bool bCompatibilityComplete = false;
        bool bLoggedWaitingForPlaylistData = false;
        bool bLoggedResolutionFailure = false;
        bool bLoggedNativeGracePeriod = false;
        bool bLoggedMissingRuntimeObjects = false;
        bool bLoggedManualFallback = false;
        bool bAllowUnconfirmedPlaylistDataResolution = false;
        bool bLoggedUnconfirmedPlaylistDataFallback = false;
        bool bLoggedModifierLookupUnavailable = false;
        bool bLoggedModifierEffectsUnavailable = false;
        bool bPlaylistPublicationInProgress = false;
        bool bPlaylistPublicationCompleted = false;
        std::vector<UFortGameplayModifierItemDefinition*>
            Modifiers;
        std::vector<FResolvedNativeLTMMutator> Mutators;
        std::unordered_map<
            UFortGameplayModifierItemDefinition*,
            double> NativeModifierRegistrationAttempts;
    };

    struct FHeistCompatibilityState
    {
        UWorld* World = nullptr;
        const UFortPlaylistAthena* Playlist = nullptr;
        bool bPlaylistPrepared = false;
        bool bAdditionalLevelsComplete = false;
        bool bLoggedMissingPlaylistLoadFunctions = false;
        bool bLoggedMissingAdditionalLevelData = false;
        bool bPublishedCompatibilityPhaseStep = false;
        uint8 PublishedCompatibilityPhaseStep =
            static_cast<uint8>(EAthenaGamePhaseStep::None);
        bool bObservedNativePhaseStep = false;
        uint8 ObservedNativePhaseStep =
            static_cast<uint8>(EAthenaGamePhaseStep::None);
        double NextPlaylistPreparationAttemptTime = 0.0;
        double NextAdditionalLevelAttemptTime = 0.0;
        double NextExitCraftDiscoveryTime = 0.0;
        std::unordered_map<AFortAthenaExitCraftSpawner*, double>
            ScheduledExitCraftSpawners;
        std::unordered_map<AFortAthenaMutator_Heist*, uint8>
            LastNotifiedPhaseSteps;
        std::unordered_map<AFortAthenaMutator_Heist*, uint8>
            ObservedNative1040PhaseSteps;
        std::unordered_set<AFortAthenaMutator_Heist*>
            CompletedNative1040SetupRecovery;
        std::unordered_set<AFortAthenaMutator_Heist*>
            ManualNative1040PhaseInvocations;
        std::unordered_map<AFortAthenaMutator_Heist*, int32>
            Native1040SetupRecoveryAttempts;
        std::unordered_map<AFortAthenaMutator_Heist*, double>
            Native1040NextSetupRecoveryAttemptTimes;
        std::unordered_map<AFortAthenaExitCraftSpawner*, double>
            Native1040DueSpawnerRecoveryTimes;
        std::unordered_map<AFortAthenaExitCraftSpawner*, int32>
            Native1040ManualSpawnAttempts;
        bool bLoggedNative1040SafeZoneUnavailable = false;
        bool bLoggedNative1040WaitingForSelection = false;
    };

    struct FDeepFriedArenaState
    {
        UWorld* World = nullptr;
        AFortAthenaMutator_Barrier* Barrier = nullptr;
        AAthenaBigBaseWall* Wall = nullptr;
        AAthenaBarrierFlag* Flags[2]{};
        AAthenaBarrierObjective* Objectives[2]{};
        TScriptInterface<IInterface> SafeZoneInterface{};
        double NextPlacementRetryTime = 0.0;
        double NextHudRefreshTime = 0.0;
        FVector LastWallLocation{};
        float LastWallYaw = 0.0f;
        int32 LastObservedWallState = -1;
        float LastObjectiveHealth[2]{-1.0f, -1.0f};
        uint8 LastPublishedDamageState[2]{9, 9};
        bool bObjectiveDestroyed[2]{};
        bool bHasWallTransform = false;
        bool bPublishedWallComingDown = false;
        bool bPublishedWallDown = false;
        bool bBindingsComplete = false;
        bool bDamageEnabled = false;
        bool bHudPublished = false;
        bool bLoggedWaitingForBarrier = false;
        bool bLoggedWaitingForObjectives = false;
        bool bLoggedWaitingForZone = false;
        bool bLoggedPlacementFailure = false;
        bool bLoggedWaitingForLava = false;
        bool bLoggedWaitingForStructuralGrid = false;
        bool bLoggedHudUnavailable = false;
    };

    struct FAshtonMiloHealthReconcileState
    {
        uint64 PassiveIdentity = 0;
        FGuid GadgetGuid{};
        int32 AppliedStoneCount = -1;
    };

    struct FAshtonCarmineHealthReconcileState
    {
        uint64 PawnIdentity = 0;
        FGuid GadgetGuid{};
        bool bRealityStoneCaptured = false;
        bool bApplied = false;
    };

    struct FAshtonMiloQuickbarReconcileState
    {
        uint64 PawnIdentity = 0;
        FGuid PrimaryGuid{};
        FGuid LauncherGuid{};
        FGuid JetpackGuid{};
    };

    struct FAuthoredNativeLTMPhaseState
    {
        UWorld* World = nullptr;
        double NextDiscoGliderReconcileTime = 0.0;
        double NextDiscoControlPointReconcileTime = 0.0;
        std::unordered_map<AFortGameplayMutator*, uint8>
            LastObservedPhaseSteps;
        std::unordered_set<AFortGameplayMutator*>
            ObservedSetup;
        std::unordered_map<AFortGameplayMutator*, uint8>
            LastObservedGamePhases;
        std::unordered_set<AFortGameplayMutator*>
            ObservedGamePhaseSetup;
        std::unordered_set<AFortGameplayMutator*>
            ManualPhaseStepInvocations;
        std::unordered_set<AFortGameplayMutator*>
            ManualGamePhaseInvocations;
        std::unordered_set<AFortPlayerControllerAthena*>
            DiscoGliderGrantedPlayers;
        std::unordered_set<AActor*>
            DiscoReplicationPreparedPoints;
        std::unordered_map<AFortGameplayMutator*, int32>
            DiscoMalformedPointObservations;
        std::unordered_set<AFortGameplayMutator*>
            AshtonDelegateBindingsComplete;
        AFortAthenaMutator_Ashton* AshtonMutator = nullptr;
        AFortAthenaMutator_InventoryOverride*
            AshtonInventoryOverride = nullptr;
        std::vector<std::pair<
            UFortItemDefinition*,
            int32>> AshtonVillainLoadout;
        std::unordered_set<const UFortItemDefinition*>
            AshtonVillainGear;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            std::unordered_map<
                const UFortItemDefinition*,
                FGuid>> AshtonInitializedVillainItems;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            std::pair<FGuid, double>>
                AshtonMiloChildrenNotBefore;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            std::pair<FGuid, double>>
                AshtonMiloRecoveryNotBefore;
        std::unordered_set<
            AFortPlayerControllerAthena*>
                AshtonMiloResetAttempted;
        std::unordered_set<
            AFortPlayerControllerAthena*>
                AshtonMiloFallbackAttempted;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            FGuid> AshtonInitializedLeaderItems;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            FGuid> AshtonInitializedLeaderBackingItems;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            AFortPlayerPawnAthena*> AshtonReadyPawns;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            FGuid> AshtonFocusedMiloPrimaryItems;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            FAshtonMiloHealthReconcileState>
                AshtonMiloHealthReconcileStates;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            FAshtonCarmineHealthReconcileState>
                AshtonCarmineHealthReconcileStates;
        std::unordered_map<
            AFortPlayerControllerAthena*,
            FAshtonMiloQuickbarReconcileState>
                AshtonMiloQuickbarReconcileStates;
        std::unordered_set<
            AFortPlayerControllerAthena*>
            AshtonEliminatedLeaders;
        AFortPlayerControllerAthena*
            AshtonStagedDeathVictim = nullptr;
        AFortPlayerControllerAthena*
            AshtonAuthorizedAllStoneFallback = nullptr;
        AFortPlayerControllerAthena*
            AshtonPromotionController = nullptr;
        AFortPlayerPawnAthena*
            AshtonPromotionPawn = nullptr;
        FGuid AshtonPromotionItemGuid{};
        double AshtonPromotionStartedAt = 0.0;
        double AshtonPromotionStartZ = 0.0;
        double AshtonPromotionReadyObservedAt = 0.0;
        double AshtonPromotionReadyFallbackAt = 0.0;
        double AshtonPromotionTeleportFallbackAt = 0.0;
        double AshtonPromotionSkydiveFallbackAt = 0.0;
        bool bAshtonPromotionUsesCarmine = false;
        bool bAshtonPromotionReadyFallbackInvoked = false;
        bool bAshtonPromotionTeleportFallbackInvoked = false;
        bool bAshtonPromotionSkydiveFallbackInvoked = false;
        bool bAshtonPromotionTeleported = false;
        bool bAshtonPromotionSkydiving = false;
        bool bAshtonLeaderPolicyInitialized = false;
        bool bAshtonLeaderVacant = true;
        uint8 AshtonVillainTeam = 255;
        double NextAshtonDependencyPreloadTime = 0.0;
        double NextAshtonPlayerReconcileTime = 0.0;
        double NextAshtonStoneReconcileTime = 0.0;
        double AshtonStoneMissingSince[6]{};
        double AshtonStoneGroundedSupplySince[6]{};
        float AshtonStoneRecoveryGeneration[6]{};
        uint8 AshtonStoneCompatibilityRestoreAttempts[6]{};
        FVector AshtonLastStoneSupplyLocation[6]{};
        bool bAshtonStoneRecoveryGenerationInitialized[6]{};
        bool bAshtonSawStoneSupply[6]{};
        bool bAshtonStoneCaptureLocked[6]{};
        bool bAshtonStoneDestroyDispatchInFlight[6]{};
        bool bLoggedAshtonStoneSpawnDeadline[6]{};
        bool bLoggedWaitingForMutator = false;
        bool bLoggedWaitingForSafeZone = false;
        bool bLoggedWaitingForAshtonInputs = false;
        bool bLoggedInvalidAshtonLoadout = false;
        bool bLoggedWaitingForDiscoGeometry = false;
        bool bLoggedMalformedDiscoPoints = false;
        bool bAshtonDependenciesLoaded = false;
    };

    struct FArsenalPlayerCompatibilityState
    {
        int32 AppliedEliminationScore = -1;
        int32 AppliedTierScore = -1;
        AFortPlayerPawnAthena* Pawn = nullptr;
        std::vector<UFortWeaponItemDefinition*> AssignedWeapons;
    };

    struct FArsenalCompatibilityState
    {
        UWorld* World = nullptr;
        AFortAthenaMutator_GG* Mutator = nullptr;
        double NextReconcileTime = 0.0;
        std::map<
            int32,
            std::vector<UFortWeaponItemDefinition*>> Tiers;
        std::unordered_set<UFortWeaponItemDefinition*> AllWeapons;
        std::unordered_map<
            AFortPlayerStateAthena*,
            FArsenalPlayerCompatibilityState> Players;
        std::unordered_map<
            AFortPlayerStateAthena*,
            AFortPlayerPawnAthena*> ClaimedEliminationPawns;
        bool bTierTableReady = false;
        bool bLoggedMissingMutator = false;
        bool bLoggedInvalidTierData = false;
    };

    struct FWaxCompatibilityState
    {
        UWorld* World = nullptr;
        AFortAthenaMutator_Wax* Mutator = nullptr;
        double MutatorResolvedTime = -1.0;
        double CompatibilityCompletedObservedTime = -1.0;
        double NextPlayerDataReconcileTime = 0.0;
        bool bCompatibilityOwnsPlayerData = false;
        bool bCompatibilityCommonDeadPawnDispatch = false;
        bool bLoggedMissingMutator = false;
        bool bLoggedInvalidPlayerData = false;
        bool bLoggedNativePlayerData = false;
        bool bBountyTokenClassesVerified = false;
        bool bLoggedInvalidBountyTokenClasses = false;
        std::unordered_map<
            AFortPlayerStateAthena*,
            TWeakObjectPtr<AFortPlayerPawnAthena>>
            ProcessedEliminationPawns;
        std::unordered_map<
            AFortPlayerStateAthena*,
            double> MissingPlayerDataSince;
    };

    FHeistCompatibilityState GHeistCompatibilityState;
    FNativeLTMCompatibilityState GNativeLTMCompatibilityState;
    FDeepFriedArenaState GDeepFriedArenaState;
    FAuthoredNativeLTMPhaseState
        GAuthoredNativeLTMPhaseState;
    FArsenalCompatibilityState GArsenalCompatibilityState;
    FWaxCompatibilityState GWaxCompatibilityState;
    bool GWaxCommonDeadPawnHookInstalled = false;
    std::unordered_set<uint64>
        GCarminePassiveReadyExecutions;
    std::unordered_set<uint64>
        GCarminePassiveTeleportExecutions;
    std::unordered_set<uint64>
        GCarminePassiveSkydiveExecutions;
    void (*GCarminePassiveReadyOG)(UObject*, FFrame&) = nullptr;
    void (*GCarminePassiveTeleportOG)(UObject*, FFrame&) = nullptr;
    void (*GCarminePassiveSkydiveOG)(UObject*, FFrame&) = nullptr;
    bool GCarminePassiveReadyExecutedThisCall = false;

    void CarminePassiveReadyOnce(
        UObject* Context,
        FFrame& Stack);
    void CarminePassiveTeleportOnce(
        UObject* Context,
        FFrame& Stack);
    void CarminePassiveSkydiveOnce(
        UObject* Context,
        FFrame& Stack);
    void ReconcileAshtonStoneHealthEffects(
        AFortGameStateAthena* GameState,
        AFortAthenaMutator_Ashton* Ashton,
        const char* Reason);
    bool EnsureCarminePassiveReadyOneShot(
        UFunction* Ready);
    bool EnsureCarminePassiveTransitionHooks(
        const UObject* Passive);
    uint64 GetCarminePassiveIdentity(
        const UObject* Passive);

    void RefreshDeepFriedArenaBindings();
    void EnsureDeepFriedArena(
        UWorld* World,
        AFortGameStateAthena* GameState,
        AFortAthenaMutator_Barrier* Barrier,
        const TScriptInterface<IInterface>& SafeZoneInterface);
    void TickDeepFriedArena(
        AFortGameStateAthena* GameState);
    void TickArsenalCompatibility(
        AFortGameStateAthena* GameState,
        double Now);
    void TickWaxCompatibility(
        AFortGameStateAthena* GameState,
        double Now);
    void ReconcileDiscoBigTeamGliders(
        AFortGameStateAthena* GameState);

    void ResetHeistCompatibilityState(
        UWorld* World,
        const UFortPlaylistAthena* Playlist = nullptr)
    {
        GHeistCompatibilityState = {};
        GHeistCompatibilityState.World = World;
        GHeistCompatibilityState.Playlist = Playlist;
    }

    void ResetNativeLTMCompatibilityState(
        UWorld* World,
        const UFortPlaylistAthena* Playlist = nullptr,
        const FNativeLTMPlaylistDescriptor* Descriptor = nullptr)
    {
        GDeepFriedArenaState = {};
        GDeepFriedArenaState.World = World;
        GAuthoredNativeLTMPhaseState = {};
        GAuthoredNativeLTMPhaseState.World = World;
        GArsenalCompatibilityState = {};
        GArsenalCompatibilityState.World = World;
        GWaxCompatibilityState = {};
        GWaxCompatibilityState.World = World;
        GNativeLTMCompatibilityState = {};
        GNativeLTMCompatibilityState.World = World;
        GNativeLTMCompatibilityState.Playlist = Playlist;
        GNativeLTMCompatibilityState.Descriptor = Descriptor;
    }

    bool EqualsInsensitive(
        const wchar_t* Left,
        const wchar_t* Right)
    {
        if (!Left || !Right)
            return false;

        std::wstring LowerLeft = Left;
        std::wstring LowerRight = Right;
        std::transform(
            LowerLeft.begin(), LowerLeft.end(),
            LowerLeft.begin(),
            [](wchar_t Character)
            {
                return static_cast<wchar_t>(std::towlower(Character));
            });
        std::transform(
            LowerRight.begin(), LowerRight.end(),
            LowerRight.begin(),
            [](wchar_t Character)
            {
                return static_cast<wchar_t>(std::towlower(Character));
            });
        return LowerLeft == LowerRight;
    }

    bool IsHeistPlaylistIdentifier(const wchar_t* Identifier)
    {
        static constexpr const wchar_t* Identifiers[] = {
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Solo.Playlist_Bling_Solo",
            L"Playlist_Bling_Solo",
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Duos.Playlist_Bling_Duos",
            L"Playlist_Bling_Duos",
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Squads.Playlist_Bling_Squads",
            L"Playlist_Bling_Squads",
            L"The Getaway",
            L"The Getaway Solos",
            L"The Getaway Duos",
            L"The Getaway Squads",
            L"Getaway"
        };

        for (const auto Candidate : Identifiers)
        {
            if (EqualsInsensitive(Identifier, Candidate))
                return true;
        }
        return false;
    }

    bool IsSaneArray(int32 Num, int32 Max, int32 Limit = 128)
    {
        return Num >= 0 && Max >= Num && Num <= Limit;
    }

    bool IsReadableArrayStorage(
        const void* Data,
        int32 Num,
        size_t ElementSize)
    {
        if (Num == 0)
            return true;
        if (!Data || Num < 0 || ElementSize == 0)
            return false;
        return SDK::MemReadable(
            Data,
            static_cast<size_t>(Num) * ElementSize);
    }

    bool IsLiveObject(const UObject* Object)
    {
        if (!Object ||
            !SDK::MemReadable(Object, sizeof(UObject)))
        {
            return false;
        }

        const int32 ObjectIndex = Object->Index;
        if (ObjectIndex < 0 ||
            ObjectIndex >= TUObjectArray::Num())
        {
            return false;
        }

        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        const int32 InvalidObjectFlags =
            Offsets::bEncryptedObjects ? 0x10200000 : 0x20;
        return Item &&
            Item->GetObject() == Object &&
            !(Item->GetFlags() & InvalidObjectFlags) &&
            Object->Class &&
            SDK::MemReadable(Object->Class, sizeof(UClass));
    }

    const FNativeLTMPlaylistDescriptor*
        FindExactNativeLTMDescriptor(
            const UFortPlaylistAthena* Playlist)
    {
        if (!IsLiveObject(Playlist))
            return nullptr;

        const auto ObjectName = Playlist->Name.ToWString();
        for (const auto& Descriptor : NativeLTMPlaylists)
        {
            if (std::wcscmp(
                    ObjectName.c_str(),
                    Descriptor.ObjectName) != 0)
            {
                continue;
            }

            // Object names alone are not unique across Unreal packages.
            // Resolving the canonical asset and comparing identity prevents a
            // failed GUI lookup (and its normal-Solos fallback) from being
            // treated as an LTM.
            const UFortPlaylistAthena* CanonicalPlaylist =
                FindObject<UFortPlaylistAthena>(Descriptor.Path);
            if (CanonicalPlaylist == Playlist)
                return &Descriptor;
        }

        return nullptr;
    }

    bool IsNativeGetawayDescriptor(
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        return Descriptor &&
            std::strcmp(
                Descriptor->ExpectedMutatorBaseName,
                "FortAthenaMutator_Heist") == 0;
    }

    bool IsNativeArsenalDescriptor(
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        return Descriptor &&
            std::strcmp(
                Descriptor->ExpectedMutatorBaseName,
                "FortAthenaMutator_GG") == 0;
    }

    bool IsNativeFoodFightDescriptor(
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        return Descriptor &&
            std::strcmp(
                Descriptor->ExpectedMutatorBaseName,
                "FortAthenaMutator_Barrier") == 0;
    }

    bool IsNativeDeepFriedDescriptor(
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        return IsNativeFoodFightDescriptor(Descriptor) &&
            std::wcscmp(
                Descriptor->ObjectName,
                L"Playlist_Barrier_16_B_Lava") == 0;
    }

    bool IsNativeWaxDescriptor(
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        return Descriptor &&
            std::strcmp(
                Descriptor->ExpectedMutatorBaseName,
                "FortAthenaMutator_Wax") == 0;
    }

    bool IsNativeAshtonDescriptor(
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        return Descriptor &&
            std::strcmp(
                Descriptor->ExpectedMutatorBaseName,
                "FortAthenaMutator_Ashton") == 0;
    }

    int32 GetAshtonStoneType(
        const UFortItemDefinition* ItemDefinition)
    {
        if (!IsLiveObject(ItemDefinition))
            return -1;

        const auto Name = ItemDefinition->Name.ToWString();
        static constexpr const wchar_t* StoneNames[] = {
            L"AshtonRockItemDef_P",
            L"AshtonRockItemDef_B",
            L"AshtonRockItemDef_R",
            L"AshtonRockItemDef_O",
            L"AshtonRockItemDef_G",
            L"AshtonRockItemDef_Y"
        };
        for (int32 StoneType = 0;
             StoneType <
                 static_cast<int32>(
                     std::size(StoneNames));
             ++StoneType)
        {
            if (Name == StoneNames[StoneType])
                return StoneType;
        }
        return -1;
    }

    bool IsAshtonStoneItemDefinition(
        const UFortItemDefinition* ItemDefinition)
    {
        return GetAshtonStoneType(ItemDefinition) >= 0;
    }

    bool IsNativeDiscoDescriptor(
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        return Descriptor &&
            std::strcmp(
                Descriptor->ExpectedMutatorBaseName,
                "FortAthenaMutator_Disco") == 0;
    }

    bool IsNativeBountyDescriptor(
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        if (!IsNativeWaxDescriptor(Descriptor))
            return false;

        return std::wcscmp(
                   Descriptor->ObjectName,
                   L"Playlist_Bounty_Solo") == 0 ||
            std::wcscmp(
                Descriptor->ObjectName,
                L"Playlist_Bounty_Duos") == 0 ||
            std::wcscmp(
                Descriptor->ObjectName,
                L"Playlist_Bounty_Squads") == 0;
    }

    UFortMutatorListComponent* GetMutatorListComponent(
        UObject* Owner)
    {
        if (!IsLiveObject(Owner))
            return nullptr;

        const int32 Offset = static_cast<int32>(
            Owner->GetOffset(
                "MutatorListComponent",
                0x0000000000010000));
        if (Offset < 0)
            return nullptr;

        auto Component =
            GetFromOffset<UFortMutatorListComponent*>(
                Owner, Offset);
        const UClass* ComponentClass =
            UFortMutatorListComponent::StaticClass();
        if (!IsLiveObject(Component) || !ComponentClass ||
            !Component->IsA(ComponentClass))
        {
            return nullptr;
        }
        return Component;
    }

    AFortGameplayMutator* InvokeGetMutatorByClass(
        UObject* Target,
        AActor* ContextActor,
        UClass* MutatorClass)
    {
        if (!Target || !MutatorClass)
            return nullptr;

        UFunction* Function =
            Target->GetFunction("GetMutatorByClass");
        if (!Function)
            return nullptr;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x40 ||
            Parameters.NameOffsetMap.size() < 2 ||
            Parameters.NameOffsetMap.size() > 3)
        {
            return nullptr;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        uint32 MutatorClassOffset = uint32(-1);
        uint32 ContextActorOffset = uint32(-1);
        uint32 ReturnValueOffset = uint32(-1);

        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (Parameter.Offset > Parameters.Size ||
                Parameter.ElementSize != sizeof(void*) ||
                sizeof(void*) >
                    static_cast<uint32>(Parameters.Size) -
                        Parameter.Offset)
            {
                return nullptr;
            }

            if (Parameter.Name == "MutatorClass")
            {
                if (!(Parameter.PropertyFlags & CPF_Parm) ||
                    (Parameter.PropertyFlags &
                        (CPF_OutParm | CPF_ReturnParm)))
                {
                    return nullptr;
                }
                MutatorClassOffset = Parameter.Offset;
            }
            else if (Parameter.Name == "ContextActor")
            {
                if (!(Parameter.PropertyFlags & CPF_Parm) ||
                    (Parameter.PropertyFlags &
                        (CPF_OutParm | CPF_ReturnParm)))
                {
                    return nullptr;
                }
                ContextActorOffset = Parameter.Offset;
            }
            else if (Parameter.Name == "ReturnValue")
            {
                if (!(Parameter.PropertyFlags & CPF_Parm) ||
                    !(Parameter.PropertyFlags & CPF_ReturnParm))
                {
                    return nullptr;
                }
                ReturnValueOffset = Parameter.Offset;
            }
            else
            {
                return nullptr;
            }
        }

        const bool bExpectsContext =
            Parameters.NameOffsetMap.size() == 3;
        if (MutatorClassOffset == uint32(-1) ||
            ReturnValueOffset == uint32(-1) ||
            bExpectsContext !=
                (ContextActorOffset != uint32(-1)))
        {
            return nullptr;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return nullptr;
        memset(Memory, 0, Parameters.Size);
        memcpy(
            static_cast<uint8*>(Memory) + MutatorClassOffset,
            &MutatorClass,
            sizeof(MutatorClass));
        if (bExpectsContext)
        {
            memcpy(
                static_cast<uint8*>(Memory) + ContextActorOffset,
                &ContextActor,
                sizeof(ContextActor));
        }

        Target->ProcessEvent(Function, Memory);
        AFortGameplayMutator* Result = nullptr;
        memcpy(
            &Result,
            static_cast<uint8*>(Memory) + ReturnValueOffset,
            sizeof(Result));
        FMemory::Free(Memory);

        const UClass* GameplayMutatorClass =
            AFortGameplayMutator::StaticClass();
        if (!IsLiveObject(Result) || !GameplayMutatorClass ||
            !Result->IsA(GameplayMutatorClass) ||
            !Result->IsA(MutatorClass))
        {
            return nullptr;
        }
        return Result;
    }

    bool InvokeSingleBoolInput(
        UObject* Target,
        const char* FunctionName,
        const char* ParameterName,
        bool Value)
    {
        if (!Target || !FunctionName || !ParameterName)
            return false;

        UFunction* Function =
            Target->GetFunction(FunctionName);
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x20 ||
            Parameters.NameOffsetMap.size() != 1)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        const auto& Parameter = Parameters.NameOffsetMap[0];
        if (Parameter.Name != ParameterName ||
            !(Parameter.PropertyFlags & CPF_Parm) ||
            (Parameter.PropertyFlags &
                (CPF_OutParm | CPF_ReturnParm)) ||
            Parameter.ElementSize != sizeof(bool) ||
            Parameter.Offset > Parameters.Size ||
            sizeof(bool) >
                static_cast<uint32>(Parameters.Size) -
                    Parameter.Offset)
        {
            return false;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return false;
        memset(Memory, 0, Parameters.Size);
        memcpy(
            static_cast<uint8*>(Memory) + Parameter.Offset,
            &Value,
            sizeof(Value));
        Target->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool InvokeSingleObjectInput(
        UObject* Target,
        const char* FunctionName,
        const char* ParameterName,
        UObject* Value)
    {
        if (!Target || !FunctionName ||
            !ParameterName || !Value)
        {
            return false;
        }

        UFunction* Function =
            Target->GetFunction(FunctionName);
        if (!Function &&
            strcmp(FunctionName, "AddMutatorToList") == 0)
        {
            const UClass* MutatorOwnerInterface =
                FindClass("FortMutatorOwner");
            const bool bImplementsMutatorOwner =
                MutatorOwnerInterface &&
                Target->GetInterface(
                    MutatorOwnerInterface) != nullptr;
            if (bImplementsMutatorOwner)
            {
                const UObject* InterfaceDefault =
                    MutatorOwnerInterface->GetDefaultObj();
                if (InterfaceDefault)
                {
                    Function =
                        InterfaceDefault->GetFunction(
                            "AddMutatorToList");
                }
                if (!Function)
                {
                    Function = const_cast<UFunction*>(
                        FindObject<UFunction>(
                            L"/Script/FortniteGame."
                            L"FortMutatorOwner."
                            L"AddMutatorToList"));
                }
            }
        }
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x20 ||
            Parameters.NameOffsetMap.size() != 1)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        const auto& Parameter = Parameters.NameOffsetMap[0];
        if (Parameter.Name != ParameterName ||
            !(Parameter.PropertyFlags & CPF_Parm) ||
            (Parameter.PropertyFlags &
                (CPF_OutParm | CPF_ReturnParm)) ||
            Parameter.ElementSize != sizeof(void*) ||
            Parameter.Offset > Parameters.Size ||
            sizeof(void*) >
                static_cast<uint32>(Parameters.Size) -
                    Parameter.Offset)
        {
            return false;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return false;
        memset(Memory, 0, Parameters.Size);
        memcpy(
            static_cast<uint8*>(Memory) + Parameter.Offset,
            &Value,
            sizeof(Value));
        Target->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool IsMutatorActive(AFortGameplayMutator* Mutator)
    {
        if (!IsLiveObject(Mutator))
            return false;
        return Mutator->HasbMutatorActive() &&
            Mutator->bMutatorActive;
    }

    bool SetMutatorActive(AFortGameplayMutator* Mutator)
    {
        if (!Mutator)
            return false;
        if (IsMutatorActive(Mutator))
            return true;

        if (!InvokeSingleBoolInput(
                Mutator,
                "SetMutatorActive",
                "bEnable",
                true))
        {
            return false;
        }
        return IsMutatorActive(Mutator);
    }

    bool IsMutatorRegistered(
        UFortMutatorListComponent* Component,
        const FResolvedNativeLTMMutator& Desired,
        AFortGameplayMutator* Mutator)
    {
        if (!Component || !Desired.Class || !Mutator ||
            !Component->HasMutators())
        {
            return false;
        }

        auto& Mutators = Component->Mutators;
        if (!IsSaneArray(
                Mutators.Num(), Mutators.Max(), 128) ||
            !IsReadableArrayStorage(
                Mutators.Data,
                Mutators.Num(),
                sizeof(AFortGameplayMutator*)))
        {
            return false;
        }

        for (int32 Index = 0; Index < Mutators.Num(); ++Index)
        {
            if (Mutators[Index] == Mutator)
                return true;
        }
        return false;
    }

    bool RegisterMutator(
        UObject* Owner,
        UFortMutatorListComponent* Component,
        const FResolvedNativeLTMMutator& Desired,
        AFortGameplayMutator* Mutator,
        bool* bUsedDirectMutation = nullptr)
    {
        if (bUsedDirectMutation)
            *bUsedDirectMutation = false;
        if (!Owner || !Component || !Desired.Class ||
            !Mutator ||
            !Component->HasMutatorDefs() ||
            !Component->HasMutators())
        {
            return false;
        }

        auto& Definitions = Component->MutatorDefs;
        auto& Mutators = Component->Mutators;
        if (!IsSaneArray(
                Definitions.Num(), Definitions.Max(), 128) ||
            !IsSaneArray(Mutators.Num(), Mutators.Max(), 128) ||
            !IsReadableArrayStorage(
                Definitions.Data,
                Definitions.Num(),
                FSoftObjectPtr::Size()) ||
            !IsReadableArrayStorage(
                Mutators.Data,
                Mutators.Num(),
                sizeof(AFortGameplayMutator*)))
        {
            return false;
        }

        if (IsMutatorRegistered(
                Component, Desired, Mutator))
        {
            return true;
        }

        // FortMutatorOwner performs the native list bookkeeping and any
        // interface-side effects.  Only touch the component arrays directly
        // if that native API did not produce a verifiable registration.
        InvokeSingleObjectInput(
            Owner,
            "AddMutatorToList",
            "Mutator",
            Mutator);
        if (IsMutatorRegistered(
                Component, Desired, Mutator))
        {
            return true;
        }

        bool bHasDefinition = false;
        for (int32 Index = 0;
             Index < Definitions.Num(); ++Index)
        {
            auto& Definition = Definitions.Get(
                Index, FSoftObjectPtr::Size());
            const UObject* ExistingObject =
                Definition.WeakPtr.Get();
            if (ExistingObject == Desired.Class ||
                (Definition.ObjectID.AssetPathName.IsValid() &&
                 Desired.Definition.ObjectID.AssetPathName.IsValid() &&
                 Definition.ObjectID.AssetPathName ==
                     Desired.Definition.ObjectID.AssetPathName))
            {
                bHasDefinition = true;
                break;
            }
        }
        if (!bHasDefinition)
        {
            Definitions.Add(
                Desired.Definition,
                FSoftObjectPtr::Size());
            if (bUsedDirectMutation)
                *bUsedDirectMutation = true;
        }

        bool bHasMutator = false;
        for (int32 Index = 0; Index < Mutators.Num(); ++Index)
        {
            if (Mutators[Index] == Mutator)
            {
                bHasMutator = true;
                break;
            }
        }
        if (!bHasMutator)
        {
            Mutators.Add(Mutator);
            if (bUsedDirectMutation)
                *bUsedDirectMutation = true;
        }

        return IsMutatorRegistered(
            Component, Desired, Mutator);
    }

    enum class EActiveGameplayModifierLookup
    {
        Unavailable,
        Missing,
        Found
    };

    EActiveGameplayModifierLookup
        InvokeGetActiveModifiers(
            AFortGameStateAthena* GameState,
            UFortGameplayModifierItemDefinition* Modifier)
    {
        if (!IsLiveObject(GameState) ||
            !IsLiveObject(Modifier))
        {
            return EActiveGameplayModifierLookup::Unavailable;
        }

        UFunction* Function =
            GameState->GetFunction("GetActiveModifiers");
        if (!Function)
            return EActiveGameplayModifierLookup::Unavailable;

        const auto Parameters = Function->GetParamsNamed();
        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        if (Parameters.Size !=
                sizeof(TArray<UFortGameplayModifierItemDefinition*>) ||
            Parameters.NameOffsetMap.size() != 1)
        {
            return EActiveGameplayModifierLookup::Unavailable;
        }

        const auto& Parameter = Parameters.NameOffsetMap[0];
        if (Parameter.Name != "OutActiveModifiers" ||
            !(Parameter.PropertyFlags & CPF_Parm) ||
            !(Parameter.PropertyFlags & CPF_OutParm) ||
            (Parameter.PropertyFlags & CPF_ReturnParm) ||
            Parameter.Offset != 0 ||
            Parameter.ElementSize !=
                sizeof(TArray<UFortGameplayModifierItemDefinition*>))
        {
            return EActiveGameplayModifierLookup::Unavailable;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return EActiveGameplayModifierLookup::Unavailable;
        memset(Memory, 0, Parameters.Size);
        GameState->ProcessEvent(Function, Memory);

        auto& ActiveModifiers =
            *static_cast<
                TArray<UFortGameplayModifierItemDefinition*>*>(
                Memory);
        EActiveGameplayModifierLookup Result =
            EActiveGameplayModifierLookup::Unavailable;
        if (IsSaneArray(
                ActiveModifiers.Num(),
                ActiveModifiers.Max(),
                128) &&
            IsReadableArrayStorage(
                ActiveModifiers.Data,
                ActiveModifiers.Num(),
                sizeof(UFortGameplayModifierItemDefinition*)))
        {
            Result = EActiveGameplayModifierLookup::Missing;
            for (auto ActiveModifier : ActiveModifiers)
            {
                if (ActiveModifier == Modifier)
                {
                    Result =
                        EActiveGameplayModifierLookup::Found;
                    break;
                }
            }
            ActiveModifiers.Free();
        }

        FMemory::Free(Memory);
        return Result;
    }

    FActiveGameplayModifierArray*
        GetActiveGameplayModifierArray(
            AFortGameStateAthena* GameState)
    {
        if (!IsLiveObject(GameState) ||
            !FActiveGameplayModifierArray::StaticStruct())
        {
            return nullptr;
        }

        const int32 Offset = static_cast<int32>(
            GameState->GetOffset(
                "ActiveGameplayModifiers"));
        if (Offset < 0)
            return nullptr;
        return &GetFromOffset<FActiveGameplayModifierArray>(
            GameState, Offset);
    }

    EActiveGameplayModifierLookup
        FindActiveGameplayModifier(
            AFortGameStateAthena* GameState,
            UFortGameplayModifierItemDefinition* Modifier)
    {
        const auto ReflectedLookup =
            InvokeGetActiveModifiers(GameState, Modifier);
        if (ReflectedLookup !=
            EActiveGameplayModifierLookup::Unavailable)
        {
            return ReflectedLookup;
        }

        auto ActiveModifiers =
            GetActiveGameplayModifierArray(GameState);
        const UStruct* ActiveModifierStruct =
            FActiveGameplayModifier::StaticStruct();
        if (!ActiveModifiers || !ActiveModifierStruct ||
            !Modifier ||
            !FActiveGameplayModifierArray::HasItems() ||
            !FActiveGameplayModifier::HasModifierDef())
        {
            return EActiveGameplayModifierLookup::Unavailable;
        }

        const int32 ElementSize =
            ActiveModifierStruct->GetPropertiesSize();
        auto& Items = ActiveModifiers->Items;
        if (ElementSize <= 0 || ElementSize > 0x200 ||
            !IsSaneArray(Items.Num(), Items.Max(), 128) ||
            !IsReadableArrayStorage(
                Items.Data, Items.Num(), ElementSize))
        {
            return EActiveGameplayModifierLookup::Unavailable;
        }

        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            auto& Item = Items.Get(Index, ElementSize);
            if (Item.ModifierDef == Modifier)
                return EActiveGameplayModifierLookup::Found;
        }
        return EActiveGameplayModifierLookup::Missing;
    }

    AFortGameplayMutator*
        FindActiveModifierOwnedMutator(
            AFortGameStateAthena* GameState,
            UFortGameplayModifierItemDefinition* Modifier,
            UClass* MutatorClass)
    {
        auto ActiveModifiers =
            GetActiveGameplayModifierArray(GameState);
        const UStruct* ActiveModifierStruct =
            FActiveGameplayModifier::StaticStruct();
        if (!ActiveModifiers || !ActiveModifierStruct ||
            !IsLiveObject(Modifier) ||
            !IsLiveObject(MutatorClass) ||
            !FActiveGameplayModifierArray::HasItems() ||
            !FActiveGameplayModifier::HasModifierDef() ||
            !FActiveGameplayModifier::HasMutators())
        {
            return nullptr;
        }

        const int32 ElementSize =
            ActiveModifierStruct->GetPropertiesSize();
        auto& Items = ActiveModifiers->Items;
        if (ElementSize <= 0 || ElementSize > 0x200 ||
            !IsSaneArray(Items.Num(), Items.Max(), 128) ||
            !IsReadableArrayStorage(
                Items.Data, Items.Num(), ElementSize))
        {
            return nullptr;
        }

        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            auto& Item = Items.Get(Index, ElementSize);
            if (Item.ModifierDef != Modifier ||
                !IsSaneArray(
                    Item.Mutators.Num(),
                    Item.Mutators.Max(),
                    128) ||
                !IsReadableArrayStorage(
                    Item.Mutators.Data,
                    Item.Mutators.Num(),
                    sizeof(AFortGameplayMutator*)))
            {
                continue;
            }

            for (auto Mutator : Item.Mutators)
            {
                if (IsLiveObject(Mutator) &&
                    Mutator->IsA(MutatorClass))
                {
                    return Mutator;
                }
            }
            return nullptr;
        }
        return nullptr;
    }

    bool InvokeRegisterGameplayModifier(
        AFortGameStateAthena* GameState,
        UFortGameplayModifierItemDefinition* Modifier)
    {
        if (!IsLiveObject(GameState) ||
            !IsLiveObject(Modifier))
            return false;

        UFunction* Function =
            GameState->GetFunction(
                "RegisterGameplayModifier");
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x40 ||
            Parameters.NameOffsetMap.size() != 3)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        uint32 ModifierOffset = uint32(-1);
        uint32 ExpirationOffset = uint32(-1);
        uint32 ReturnValueOffset = uint32(-1);
        for (const auto& Parameter :
             Parameters.NameOffsetMap)
        {
            if (Parameter.Offset > Parameters.Size ||
                Parameter.ElementSize >
                    static_cast<uint32>(Parameters.Size) -
                        Parameter.Offset)
            {
                return false;
            }

            if (Parameter.Name == "InModifierToRegister")
            {
                if (!(Parameter.PropertyFlags & CPF_Parm) ||
                    (Parameter.PropertyFlags &
                        (CPF_OutParm | CPF_ReturnParm)) ||
                    Parameter.ElementSize != sizeof(void*))
                {
                    return false;
                }
                ModifierOffset = Parameter.Offset;
            }
            else if (Parameter.Name == "Expiration")
            {
                if (!(Parameter.PropertyFlags & CPF_Parm) ||
                    (Parameter.PropertyFlags &
                        (CPF_OutParm | CPF_ReturnParm)) ||
                    Parameter.ElementSize != sizeof(int32))
                {
                    return false;
                }
                ExpirationOffset = Parameter.Offset;
            }
            else if (Parameter.Name == "ReturnValue")
            {
                if (!(Parameter.PropertyFlags & CPF_Parm) ||
                    !(Parameter.PropertyFlags & CPF_ReturnParm) ||
                    Parameter.ElementSize != sizeof(int32))
                {
                    return false;
                }
                ReturnValueOffset = Parameter.Offset;
            }
            else
            {
                return false;
            }
        }

        if (ModifierOffset == uint32(-1) ||
            ExpirationOffset == uint32(-1) ||
            ReturnValueOffset == uint32(-1))
        {
            return false;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return false;
        memset(Memory, 0, Parameters.Size);
        memcpy(
            static_cast<uint8*>(Memory) + ModifierOffset,
            &Modifier,
            sizeof(Modifier));
        const int32 Expiration = 0;
        memcpy(
            static_cast<uint8*>(Memory) + ExpirationOffset,
            &Expiration,
            sizeof(Expiration));
        GameState->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool AllGameplayModifiersAreRegistered(
        AFortGameStateAthena* GameState)
    {
        if (!GameState ||
            GNativeLTMCompatibilityState.Modifiers.empty())
        {
            return false;
        }

        for (auto Modifier :
             GNativeLTMCompatibilityState.Modifiers)
        {
            if (FindActiveGameplayModifier(
                    GameState, Modifier) !=
                EActiveGameplayModifierLookup::Found)
            {
                return false;
            }
        }
        return true;
    }

    AFortGameplayMutator* FindWorldMutator(
        UClass* MutatorClass)
    {
        if (!MutatorClass)
            return nullptr;

        TArray<AActor*> Actors;
        Utils::GetAll(MutatorClass, Actors);
        AFortGameplayMutator* Result = nullptr;
        if (IsSaneArray(
                Actors.Num(), Actors.Max(), 128) &&
            IsReadableArrayStorage(
                Actors.Data,
                Actors.Num(),
                sizeof(AActor*)))
        {
            for (auto Actor : Actors)
            {
                if (Actor &&
                    SDK::MemReadable(
                        Actor, sizeof(UObject)) &&
                    Actor->IsA(MutatorClass))
                {
                    Result =
                        static_cast<AFortGameplayMutator*>(Actor);
                    break;
                }
            }
        }
        Actors.Free();
        return Result;
    }

    AFortGameplayMutator* FindExistingMutator(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        UFortMutatorListComponent* GameModeComponent,
        UFortMutatorListComponent* GameStateComponent,
        const FResolvedNativeLTMMutator& Desired)
    {
        UClass* MutatorClass = Desired.Class;
        if (!GameMode || !GameState || !MutatorClass)
            return nullptr;

        if (auto Existing =
                FindActiveModifierOwnedMutator(
                    GameState,
                    Desired.Modifier,
                    MutatorClass))
        {
            return Existing;
        }

        if (auto Existing = InvokeGetMutatorByClass(
                GameModeComponent, nullptr, MutatorClass))
        {
            return Existing;
        }
        if (auto Existing = InvokeGetMutatorByClass(
                GameStateComponent, nullptr, MutatorClass))
        {
            return Existing;
        }
        if (auto Existing = InvokeGetMutatorByClass(
                GameMode, GameMode, MutatorClass))
        {
            return Existing;
        }
        if (auto Existing = InvokeGetMutatorByClass(
                GameState, GameMode, MutatorClass))
        {
            return Existing;
        }
        return FindWorldMutator(MutatorClass);
    }

    bool NativeMutatorActorsAreComplete(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        UFortMutatorListComponent* GameModeComponent,
        UFortMutatorListComponent* GameStateComponent)
    {
        if (!GameMode || !GameState ||
            !GameModeComponent || !GameStateComponent ||
            GNativeLTMCompatibilityState.Mutators.empty())
        {
            return false;
        }

        for (const auto& Desired :
             GNativeLTMCompatibilityState.Mutators)
        {
            auto Existing = FindExistingMutator(
                GameMode,
                GameState,
                GameModeComponent,
                GameStateComponent,
                Desired);
            if (!Existing ||
                !IsMutatorActive(Existing) ||
                (Desired.bMutatesGameMode &&
                 !IsMutatorRegistered(
                     GameModeComponent,
                     Desired,
                     Existing)) ||
                (Desired.bMutatesGameState &&
                 !IsMutatorRegistered(
                     GameStateComponent,
                     Desired,
                     Existing)))
            {
                return false;
            }
        }
        return true;
    }

    bool NativeMutatorSetIsComplete(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        UFortMutatorListComponent* GameModeComponent,
        UFortMutatorListComponent* GameStateComponent)
    {
        return NativeMutatorActorsAreComplete(
                   GameMode,
                   GameState,
                   GameModeComponent,
                   GameStateComponent) &&
            AllGameplayModifiersAreRegistered(GameState);
    }

    AFortGameplayMutator* SpawnConfiguredMutator(
        UWorld* World,
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        const FResolvedNativeLTMMutator& Desired)
    {
        if (!World || !GameMode || !GameState ||
            !Desired.Class)
        {
            return nullptr;
        }

        auto Spawned =
            World->SpawnActorUnfinished<AFortGameplayMutator>(
                Desired.Class,
                FVector(),
                FRotator(),
                GameMode);
        const UClass* GameplayMutatorClass =
            AFortGameplayMutator::StaticClass();
        if (!Spawned || !GameplayMutatorClass ||
            !Spawned->IsA(GameplayMutatorClass) ||
            !Spawned->IsA(Desired.Class))
        {
            if (Spawned)
                Spawned->K2_DestroyActor();
            return nullptr;
        }

        const UClass* AthenaMutatorClass =
            AFortAthenaMutator::StaticClass();
        if (AthenaMutatorClass &&
            Spawned->IsA(AthenaMutatorClass))
        {
            auto AthenaMutator =
                static_cast<AFortAthenaMutator*>(Spawned);
            if (AthenaMutator->HasCachedGameMode())
                AthenaMutator->CachedGameMode = GameMode;
            if (AthenaMutator->HasCachedGameState())
                AthenaMutator->CachedGameState = GameState;
        }

        Spawned = World->FinishSpawnActor<AFortGameplayMutator>(
            Spawned, FVector(), FRotator());
        if (!Spawned || !Spawned->IsA(Desired.Class))
            return nullptr;

        if (AthenaMutatorClass &&
            Spawned->IsA(AthenaMutatorClass))
        {
            auto AthenaMutator =
                static_cast<AFortAthenaMutator*>(Spawned);
            if (AthenaMutator->HasCachedGameMode())
                AthenaMutator->CachedGameMode = GameMode;
            if (AthenaMutator->HasCachedGameState())
                AthenaMutator->CachedGameState = GameState;
        }

        return Spawned;
    }

    bool IsValidatedNativeFunction(uintptr_t Address)
    {
        if (!Address)
            return false;

        auto TextSection =
            Memcury::PE::Section::GetSection(".text");
        const uintptr_t TextStart =
            TextSection.GetSectionStart().Get();
        const uintptr_t TextEnd =
            TextSection.GetSectionEnd().Get();
        if (!TextStart || TextEnd <= TextStart ||
            Address < TextStart || Address >= TextEnd)
        {
            return false;
        }

        const uintptr_t ModuleBase = Memcury::PE::GetModuleBase();
        const auto NtHeaders = Memcury::PE::GetNTHeaders();
        if (!ModuleBase || !NtHeaders)
            return false;

        constexpr uintptr_t RequiredReadableBytes = 16;
        const uintptr_t ModuleSize =
            NtHeaders->OptionalHeader.SizeOfImage;
        if (ModuleSize < RequiredReadableBytes ||
            Address < ModuleBase ||
            Address - ModuleBase >
                ModuleSize - RequiredReadableBytes)
        {
            return false;
        }

        return SDK::MemReadable(
            reinterpret_cast<void*>(Address),
            RequiredReadableBytes);
    }

    uintptr_t FindNativeLoadCurrentPlaylistData()
    {
        static bool bInitialized = false;
        static uintptr_t Address = 0;
        if (bInitialized)
            return Address;
        bInitialized = true;

        if (!FFortAthenaHeistCompatibility::IsSupportedBuild())
            return 0;

        auto StringRef = Memcury::Scanner::FindStringRef(
            L"PLAYLIST: Playlist Object is loading its assets in "
            L"AFortGameStateAthena::LoadCurrentPlaylistData(), "
            L"PlaylistName is %s (Server Side)",
            false);
        if (!StringRef.IsValid())
        {
            StringRef = Memcury::Scanner::FindStringRef(
                L"PLAYLIST: Playlist Object is loading its assets in "
                L"AFortGameStateAthena::LoadCurrentPlaylistData(), "
                L"PlaylistName is %s (Client Side)",
                false);
        }

        if (StringRef.IsValid())
        {
            const uintptr_t ReferenceAddress = StringRef.Get();
            const uintptr_t Candidate =
                StringRef.FindFunctionBoundary(false).Get();
            if (IsValidatedNativeFunction(Candidate) &&
                Candidate <= ReferenceAddress &&
                ReferenceAddress - Candidate <= 2048)
            {
                Address = Candidate;
            }
        }

        SDK::DbgLog(
            "[Heist] native LoadCurrentPlaylistData resolver=%p\n",
            reinterpret_cast<void*>(Address));
        return Address;
    }

    uintptr_t FindNativeInitializePlaylistDataPreDataLoad()
    {
        static bool bInitialized = false;
        static uintptr_t Address = 0;
        if (bInitialized)
            return Address;
        bInitialized = true;

        if (!FFortAthenaHeistCompatibility::IsSupportedBuild())
            return 0;

        const uintptr_t Candidate =
            Memcury::Scanner::FindPattern(
                "40 53 48 83 EC ? 48 8B D9 48 8B 89 ? ? ? ? "
                "48 85 C9 74 ? 80 BB",
                false)
                .Get();
        if (IsValidatedNativeFunction(Candidate))
            Address = Candidate;

        SDK::DbgLog(
            "[Heist] native InitializePlaylistDataPreDataLoad "
            "resolver=%p\n",
            reinterpret_cast<void*>(Address));
        return Address;
    }

    std::vector<AFortAthenaMutator_Heist*> FindHeistMutators(
        AFortGameStateAthena* GameState)
    {
        std::vector<AFortAthenaMutator_Heist*> Result;
        const UClass* HeistClass = AFortAthenaMutator_Heist::StaticClass();
        if (!GameState || !HeistClass)
            return Result;

        auto AddUnique =
            [&](AActor* Candidate)
            {
                if (!IsLiveObject(Candidate) ||
                    !Candidate->IsA(HeistClass))
                    return;

                auto Heist = static_cast<AFortAthenaMutator_Heist*>(Candidate);
                if (std::find(Result.begin(), Result.end(), Heist) ==
                    Result.end())
                {
                    Result.push_back(Heist);
                }
            };

        if (GameState->HasGameplayMutators())
        {
            auto& Mutators = GameState->GameplayMutators;
            if (IsSaneArray(Mutators.Num(), Mutators.Max(), 64))
            {
                for (int32 Index = 0; Index < Mutators.Num(); ++Index)
                    AddUnique(Mutators[Index]);
            }
        }

        if (Result.empty())
        {
            TArray<AActor*> HeistActors;
            Utils::GetAll(HeistClass, HeistActors);
            if (IsSaneArray(HeistActors.Num(), HeistActors.Max(), 64))
            {
                for (auto HeistActor : HeistActors)
                    AddUnique(HeistActor);
            }
            HeistActors.Free();
        }

        return Result;
    }

    bool HasStreamedPlaylistLevel(
        AFortGameStateAthena* GameState,
        const FName& LevelName)
    {
        if (!GameState || !GameState->HasAdditionalPlaylistLevelsStreamed())
            return false;

        auto& StreamedLevels = GameState->AdditionalPlaylistLevelsStreamed;
        if (!IsSaneArray(
                StreamedLevels.Num(), StreamedLevels.Max(), 256))
        {
            return false;
        }

        const UStruct* AdditionalLevelStruct =
            FAdditionalLevelStreamed::StaticStruct();
        if (AdditionalLevelStruct)
        {
            const int32 StructSize =
                AdditionalLevelStruct->GetPropertiesSize();
            if (StructSize <= 0 || StructSize > 0x100)
                return false;

            for (int32 Index = 0; Index < StreamedLevels.Num(); ++Index)
            {
                auto& Existing =
                    StreamedLevels.Get(Index, StructSize);
                if (FAdditionalLevelStreamed::HasLevelName() &&
                    Existing.LevelName == LevelName)
                {
                    return true;
                }
            }
            return false;
        }

        auto& LegacyLevels =
            reinterpret_cast<TArray<FName>&>(StreamedLevels);
        for (int32 Index = 0; Index < LegacyLevels.Num(); ++Index)
        {
            if (LegacyLevels[Index] == LevelName)
                return true;
        }
        return false;
    }

    bool AddStreamedPlaylistLevel(
        AFortGameStateAthena* GameState,
        const FName& LevelName,
        bool bServerOnly)
    {
        if (!GameState ||
            !GameState->HasAdditionalPlaylistLevelsStreamed() ||
            HasStreamedPlaylistLevel(GameState, LevelName))
        {
            return false;
        }

        auto& StreamedLevels = GameState->AdditionalPlaylistLevelsStreamed;
        const UStruct* AdditionalLevelStruct =
            FAdditionalLevelStreamed::StaticStruct();
        if (!AdditionalLevelStruct)
        {
            reinterpret_cast<TArray<FName>&>(StreamedLevels).Add(LevelName);
            return true;
        }

        const int32 StructSize =
            AdditionalLevelStruct->GetPropertiesSize();
        if (StructSize <= 0 || StructSize > 0x100 ||
            !FAdditionalLevelStreamed::HasLevelName() ||
            !FAdditionalLevelStreamed::HasbIsServerOnly())
        {
            return false;
        }

        void* Memory = FMemory::Malloc(StructSize);
        if (!Memory)
            return false;
        memset(Memory, 0, StructSize);

        auto Level = static_cast<FAdditionalLevelStreamed*>(Memory);
        FName MutableLevelName = LevelName;
        Level->LevelName = MutableLevelName;
        Level->bIsServerOnly = bServerOnly;
        StreamedLevels.Add(*Level, StructSize);
        FMemory::Free(Memory);
        return true;
    }

    bool HasNoParameters(UFunction* Function)
    {
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        return Parameters.Size == 0 &&
            Parameters.NameOffsetMap.empty();
    }

    bool HasNoInputBoolReturn(UFunction* Function)
    {
        if (!Function)
            return false;

        const auto Parameters =
            Function->GetParamsNamed();
        if (Parameters.Size <= 0 ||
            Parameters.Size > 0x20 ||
            Parameters.NameOffsetMap.size() != 1)
        {
            return false;
        }

        constexpr uint64 CPF_Parm =
            0x0000000000000080;
        constexpr uint64 CPF_ReturnParm =
            0x0000000000000400;
        const auto& ReturnValue =
            Parameters.NameOffsetMap[0];
        return
            (ReturnValue.PropertyFlags & CPF_Parm) &&
            (ReturnValue.PropertyFlags &
                CPF_ReturnParm) &&
            ReturnValue.ElementSize == sizeof(bool) &&
            ReturnValue.Offset <= Parameters.Size &&
            sizeof(bool) <=
                static_cast<uint32>(
                    Parameters.Size) -
                    ReturnValue.Offset;
    }

    bool HasSingleByteInputParameter(UFunction* Function)
    {
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x20 ||
            Parameters.NameOffsetMap.size() != 1)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        const auto& Parameter = Parameters.NameOffsetMap[0];
        return (Parameter.PropertyFlags & CPF_Parm) &&
            !(Parameter.PropertyFlags &
                (CPF_OutParm | CPF_ReturnParm)) &&
            Parameter.ElementSize == sizeof(uint8) &&
            Parameter.Offset <= Parameters.Size &&
            sizeof(uint8) <=
                static_cast<uint32>(Parameters.Size) -
                    Parameter.Offset;
    }

    bool CallReflectedNoParams(UObject* Object, const char* FunctionName)
    {
        if (!Object || !FunctionName)
            return false;

        UFunction* Function = Object->GetFunction(FunctionName);
        if (!HasNoParameters(Function))
            return false;

        Object->Call<void>(Function);
        return true;
    }

    bool CallReflectedByteParam(
        UObject* Object,
        const char* FunctionName,
        uint8 Value)
    {
        if (!Object || !FunctionName)
            return false;

        UFunction* Function = Object->GetFunction(FunctionName);
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x20 ||
            Parameters.NameOffsetMap.size() != 1)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        const auto& Parameter = Parameters.NameOffsetMap[0];
        if (!(Parameter.PropertyFlags & CPF_Parm) ||
            (Parameter.PropertyFlags &
                (CPF_OutParm | CPF_ReturnParm)) ||
            Parameter.ElementSize != sizeof(Value) ||
            Parameter.Offset > Parameters.Size ||
            sizeof(Value) >
                static_cast<uint32>(Parameters.Size) -
                    Parameter.Offset)
        {
            return false;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return false;
        memset(Memory, 0, Parameters.Size);
        memcpy(
            static_cast<uint8*>(Memory) + Parameter.Offset,
            &Value,
            sizeof(Value));
        Object->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool InvokeGamePhaseStep(
        UObject* Target,
        UFunction* Function,
        uint8 NewStep)
    {
        if (!Target || !Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x40 ||
            Parameters.NameOffsetMap.size() != 1)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        const auto& Parameter = Parameters.NameOffsetMap[0];
        // Delegate-compatible functions can choose their own parameter name.
        // The exact one-byte input layout is the ABI contract that matters.
        if (!(Parameter.PropertyFlags & CPF_Parm) ||
            (Parameter.PropertyFlags &
                (CPF_OutParm | CPF_ReturnParm)) ||
            Parameter.ElementSize != sizeof(uint8) ||
            Parameter.Offset > Parameters.Size ||
            sizeof(uint8) >
                static_cast<uint32>(Parameters.Size) -
                    Parameter.Offset)
        {
            return false;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return false;
        memset(Memory, 0, Parameters.Size);
        memcpy(
            static_cast<uint8*>(Memory) + Parameter.Offset,
            &NewStep,
            sizeof(NewStep));
        Target->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool ResolveNative1040PhaseLayout(
        UFunction* Function,
        uint32& ParametersSize,
        uint32& SafeZoneInterfaceOffset,
        uint32& GamePhaseStepOffset)
    {
        ParametersSize = 0;
        SafeZoneInterfaceOffset = uint32(-1);
        GamePhaseStepOffset = uint32(-1);
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x40 ||
            Parameters.NameOffsetMap.size() != 2)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) ||
                (Parameter.PropertyFlags & CPF_ReturnParm) ||
                Parameter.Offset > Parameters.Size)
            {
                return false;
            }

            if (Parameter.Name == "SafeZoneInterface")
            {
                // This is a const-reference interface parameter, so Unreal
                // legitimately marks it as an OutParm.
                if (Parameter.ElementSize !=
                        sizeof(TScriptInterface<IInterface>) ||
                    sizeof(TScriptInterface<IInterface>) >
                        static_cast<uint32>(Parameters.Size) -
                            Parameter.Offset)
                {
                    return false;
                }
                SafeZoneInterfaceOffset = Parameter.Offset;
            }
            else if (Parameter.Name == "GamePhaseStep")
            {
                if ((Parameter.PropertyFlags & CPF_OutParm) ||
                    Parameter.ElementSize != sizeof(uint8) ||
                    sizeof(uint8) >
                        static_cast<uint32>(Parameters.Size) -
                            Parameter.Offset)
                {
                    return false;
                }
                GamePhaseStepOffset = Parameter.Offset;
            }
            else
            {
                return false;
            }
        }

        if (SafeZoneInterfaceOffset == uint32(-1) ||
            GamePhaseStepOffset == uint32(-1) ||
            SafeZoneInterfaceOffset == GamePhaseStepOffset)
        {
            return false;
        }

        ParametersSize = static_cast<uint32>(Parameters.Size);
        return true;
    }

    bool ResolveRelevantSafeZoneInterface(
        AFortGameStateAthena* GameState,
        TScriptInterface<IInterface>& OutInterface)
    {
        OutInterface = {};
        if (!IsLiveObject(GameState))
            return false;

        const UClass* LibraryClass =
            FindClass("FortSafeZoneBlueprintLibrary");
        UObject* LibraryDefault =
            LibraryClass
                ? const_cast<UClass*>(LibraryClass)->GetDefaultObj()
                : nullptr;
        UFunction* Function =
            LibraryDefault
                ? LibraryDefault->GetFunction(
                    "GetRelevantSafeZoneInterface")
                : nullptr;
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x40 ||
            Parameters.NameOffsetMap.size() != 2)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        uint32 PlayerContextOffset = uint32(-1);
        uint32 ReturnValueOffset = uint32(-1);
        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (Parameter.Offset > Parameters.Size)
                return false;

            if (Parameter.Name == "PlayerContext")
            {
                if (!(Parameter.PropertyFlags & CPF_Parm) ||
                    (Parameter.PropertyFlags &
                        (CPF_OutParm | CPF_ReturnParm)) ||
                    Parameter.ElementSize != sizeof(void*) ||
                    sizeof(void*) >
                        static_cast<uint32>(Parameters.Size) -
                            Parameter.Offset)
                {
                    return false;
                }
                PlayerContextOffset = Parameter.Offset;
            }
            else if (Parameter.Name == "ReturnValue")
            {
                if (!(Parameter.PropertyFlags & CPF_Parm) ||
                    !(Parameter.PropertyFlags & CPF_OutParm) ||
                    !(Parameter.PropertyFlags & CPF_ReturnParm) ||
                    Parameter.ElementSize !=
                        sizeof(TScriptInterface<IInterface>) ||
                    sizeof(TScriptInterface<IInterface>) >
                        static_cast<uint32>(Parameters.Size) -
                            Parameter.Offset)
                {
                    return false;
                }
                ReturnValueOffset = Parameter.Offset;
            }
            else
            {
                return false;
            }
        }
        if (PlayerContextOffset == uint32(-1) ||
            ReturnValueOffset == uint32(-1))
        {
            return false;
        }

        AFortPlayerStateAthena* PlayerContext = nullptr;
        if (GameState->HasPlayerArray())
        {
            auto& Players = GameState->PlayerArray;
            if (IsSaneArray(Players.Num(), Players.Max(), 256))
            {
                for (auto Candidate : Players)
                {
                    if (IsLiveObject(Candidate))
                    {
                        PlayerContext = Candidate;
                        break;
                    }
                }
            }
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return false;
        memset(Memory, 0, Parameters.Size);
        memcpy(
            static_cast<uint8*>(Memory) + PlayerContextOffset,
            &PlayerContext,
            sizeof(PlayerContext));
        LibraryDefault->ProcessEvent(Function, Memory);
        memcpy(
            &OutInterface,
            static_cast<uint8*>(Memory) + ReturnValueOffset,
            sizeof(OutInterface));
        FMemory::Free(Memory);

        // Blueprint-only implementations can legally have a null native
        // InterfacePointer; the UObject remains callable through ProcessEvent.
        return IsLiveObject(
            const_cast<UObject*>(
                OutInterface.ObjectPointer));
    }

    bool InvokeNative1040HeistGamePhaseStep(
        AFortAthenaMutator_Heist* Heist,
        const TScriptInterface<IInterface>& SafeZoneInterface,
        uint8 NewStep)
    {
        if (!IsLiveObject(Heist) ||
            !IsLiveObject(
                const_cast<UObject*>(
                    SafeZoneInterface.ObjectPointer)))
        {
            return false;
        }

        UFunction* Function =
            Heist->GetFunction("OnGamePhaseStepChanged");
        uint32 ParametersSize = 0;
        uint32 SafeZoneInterfaceOffset = 0;
        uint32 GamePhaseStepOffset = 0;
        if (!ResolveNative1040PhaseLayout(
                Function,
                ParametersSize,
                SafeZoneInterfaceOffset,
                GamePhaseStepOffset))
        {
            return false;
        }

        void* Memory = FMemory::Malloc(ParametersSize);
        if (!Memory)
            return false;
        memset(Memory, 0, ParametersSize);
        memcpy(
            static_cast<uint8*>(Memory) +
                SafeZoneInterfaceOffset,
            &SafeZoneInterface,
            sizeof(SafeZoneInterface));
        memcpy(
            static_cast<uint8*>(Memory) +
                GamePhaseStepOffset,
            &NewStep,
            sizeof(NewStep));
        GHeistCompatibilityState
            .ManualNative1040PhaseInvocations.insert(Heist);
        Heist->ProcessEvent(Function, Memory);
        GHeistCompatibilityState
            .ManualNative1040PhaseInvocations.erase(Heist);
        FMemory::Free(Memory);
        return true;
    }

    bool InvokeAuthoredNativeLTMGamePhaseStep(
        AFortGameplayMutator* Mutator,
        const TScriptInterface<IInterface>& SafeZoneInterface,
        uint8 NewStep)
    {
        if (!IsLiveObject(Mutator) ||
            !IsLiveObject(
                const_cast<UObject*>(
                    SafeZoneInterface.ObjectPointer)))
        {
            return false;
        }

        UFunction* Function =
            Mutator->GetFunction("OnGamePhaseStepChanged");
        uint32 ParametersSize = 0;
        uint32 SafeZoneInterfaceOffset = 0;
        uint32 GamePhaseStepOffset = 0;
        if (!ResolveNative1040PhaseLayout(
                Function,
                ParametersSize,
                SafeZoneInterfaceOffset,
                GamePhaseStepOffset))
        {
            return false;
        }

        void* Memory = FMemory::Malloc(ParametersSize);
        if (!Memory)
            return false;
        memset(Memory, 0, ParametersSize);
        memcpy(
            static_cast<uint8*>(Memory) +
                SafeZoneInterfaceOffset,
            &SafeZoneInterface,
            sizeof(SafeZoneInterface));
        memcpy(
            static_cast<uint8*>(Memory) +
                GamePhaseStepOffset,
            &NewStep,
            sizeof(NewStep));

        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        PhaseState.ManualPhaseStepInvocations.insert(
            Mutator);
        Mutator->ProcessEvent(Function, Memory);
        PhaseState.ManualPhaseStepInvocations.erase(
            Mutator);
        FMemory::Free(Memory);

        PhaseState.LastObservedPhaseSteps[Mutator] =
            NewStep;
        if (NewStep ==
            static_cast<uint8>(
                EAthenaGamePhaseStep::Setup))
        {
            PhaseState.ObservedSetup.insert(Mutator);
        }
        return true;
    }

    bool InvokeAuthoredNativeLTMGamePhase(
        AFortGameplayMutator* Mutator,
        uint8 NewPhase)
    {
        if (!IsLiveObject(Mutator))
            return false;

        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        PhaseState.ManualGamePhaseInvocations.insert(
            Mutator);
        const bool bInvoked =
            CallReflectedByteParam(
                Mutator,
                "OnGamePhaseChanged",
                NewPhase);
        PhaseState.ManualGamePhaseInvocations.erase(
            Mutator);
        if (!bInvoked)
            return false;

        PhaseState.LastObservedGamePhases[Mutator] =
            NewPhase;
        if (NewPhase ==
            static_cast<uint8>(EAthenaGamePhase::Setup))
        {
            PhaseState.ObservedGamePhaseSetup.insert(
                Mutator);
        }
        return true;
    }

    bool ResolveExitCraftCallbackLayout(
        UFunction* Function,
        uint32& ParametersSize,
        uint32& ExitCraftOffset,
        uint32& ExitCraftSpawnerOffset)
    {
        ParametersSize = 0;
        ExitCraftOffset = uint32(-1);
        ExitCraftSpawnerOffset = uint32(-1);
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x80 ||
            Parameters.NameOffsetMap.size() != 2)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) ||
                (Parameter.PropertyFlags &
                    (CPF_OutParm | CPF_ReturnParm)) ||
                Parameter.ElementSize != sizeof(void*) ||
                Parameter.Offset > Parameters.Size ||
                sizeof(void*) >
                    static_cast<uint32>(Parameters.Size) -
                        Parameter.Offset)
            {
                return false;
            }

            uint32* Destination = nullptr;
            if (Parameter.Name == "ExitCraft")
                Destination = &ExitCraftOffset;
            else if (Parameter.Name == "ExitCraftSpawner")
                Destination = &ExitCraftSpawnerOffset;
            else
                return false;

            if (*Destination != uint32(-1))
                return false;
            *Destination = Parameter.Offset;
        }

        if (ExitCraftOffset == uint32(-1) ||
            ExitCraftSpawnerOffset == uint32(-1) ||
            ExitCraftOffset == ExitCraftSpawnerOffset)
        {
            return false;
        }

        ParametersSize = static_cast<uint32>(Parameters.Size);
        return true;
    }

    bool CanInvokeExitCraftSpawned(UFunction* Function)
    {
        uint32 ParametersSize = 0;
        uint32 ExitCraftOffset = 0;
        uint32 ExitCraftSpawnerOffset = 0;
        return ResolveExitCraftCallbackLayout(
            Function,
            ParametersSize,
            ExitCraftOffset,
            ExitCraftSpawnerOffset);
    }

    bool InvokeExitCraftSpawned(
        UObject* Target,
        UFunction* Function,
        AFortAthenaExitCraft* ExitCraft,
        AFortAthenaExitCraftSpawner* ExitCraftSpawner)
    {
        if (!Target)
            return false;

        uint32 ParametersSize = 0;
        uint32 ExitCraftOffset = 0;
        uint32 ExitCraftSpawnerOffset = 0;
        if (!ResolveExitCraftCallbackLayout(
                Function,
                ParametersSize,
                ExitCraftOffset,
                ExitCraftSpawnerOffset))
        {
            // The 10.40 ABI is a fixed pair of plain UObject pointers at
            // offsets 0x0 and 0x8. Reflection metadata can be incomplete on
            // stripped servers even though the native function is callable.
            if (FFortAthenaNativeLTMCompatibility::
                    IsSupportedBuild() &&
                Function)
            {
                struct FNative1040ExitCraftSpawnedParams
                {
                    AFortAthenaExitCraft* ExitCraft;
                    AFortAthenaExitCraftSpawner*
                        ExitCraftSpawner;
                } Params{ExitCraft, ExitCraftSpawner};
                static_assert(
                    sizeof(Params) == 0x10,
                    "Unexpected 10.40 exit-craft callback ABI");
                Target->ProcessEvent(Function, &Params);
                SDK::DbgLog(
                    "[Getaway1040] invoked OnExitCraftSpawned "
                    "through its fixed 0x10 ABI fallback\n");
                return true;
            }
            return false;
        }

        void* Memory = FMemory::Malloc(ParametersSize);
        if (!Memory)
            return false;
        memset(Memory, 0, ParametersSize);
        memcpy(
            static_cast<uint8*>(Memory) + ExitCraftOffset,
            &ExitCraft,
            sizeof(ExitCraft));
        memcpy(
            static_cast<uint8*>(Memory) + ExitCraftSpawnerOffset,
            &ExitCraftSpawner,
            sizeof(ExitCraftSpawner));
        Target->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool BroadcastGamePhaseStepChanged(
        AFortGameStateAthena* GameState,
        uint8 NewStep,
        std::unordered_set<UObject*>& InvokedTargets)
    {
        if (!GameState || !GameState->HasGamePhaseStepChanged())
            return false;

        auto& InvocationList =
            GameState->GamePhaseStepChanged.InvocationList;
        if (!IsSaneArray(
                InvocationList.Num(), InvocationList.Max(), 64) ||
            InvocationList.Num() == 0)
        {
            return false;
        }

        struct FPhaseDelegateSnapshot
        {
            FWeakObjectPtr Object;
            FName FunctionName;
        };
        std::vector<FPhaseDelegateSnapshot> Delegates;
        Delegates.reserve(InvocationList.Num());
        for (int32 Index = 0; Index < InvocationList.Num(); ++Index)
        {
            auto& Delegate =
                InvocationList.Get(Index, FScriptDelegate::Size());
            Delegates.push_back(
                { Delegate.Object, Delegate.FunctionName });
        }

        bool bInvoked = false;
        for (const auto& Delegate : Delegates)
        {
            auto Target =
                const_cast<UObject*>(Delegate.Object.Get());
            if (!Target ||
                !SDK::MemReadable(Target, sizeof(UObject)))
            {
                continue;
            }

            UFunction* Function =
                Target->GetFunction(Delegate.FunctionName);
            if (!InvokeGamePhaseStep(Target, Function, NewStep))
                continue;

            InvokedTargets.insert(Target);
            bInvoked = true;
        }

        return bInvoked;
    }

    uint8 DeriveGamePhaseStep(
        AFortGameStateAthena* GameState,
        float Now,
        float& TimeRemaining)
    {
        TimeRemaining = 0.0f;
        if (!GameState || !GameState->HasGamePhase())
            return static_cast<uint8>(EAthenaGamePhaseStep::None);

        switch (static_cast<EAthenaGamePhase>(GameState->GamePhase))
        {
        case EAthenaGamePhase::Setup:
            return static_cast<uint8>(EAthenaGamePhaseStep::Setup);

        case EAthenaGamePhase::Warmup:
            if (GameState->HasWarmupCountdownStartTime() &&
                GameState->WarmupCountdownStartTime < 0.0f)
            {
                return static_cast<uint8>(EAthenaGamePhaseStep::Setup);
            }
            if (GameState->HasWarmupCountdownEndTime() &&
                GameState->WarmupCountdownEndTime > Now)
            {
                TimeRemaining =
                    (std::max)(
                        GameState->WarmupCountdownEndTime - Now,
                        0.0f);
                return static_cast<uint8>(
                    TimeRemaining <= 10.0f
                        ? EAthenaGamePhaseStep::GetReady
                        : EAthenaGamePhaseStep::Warmup);
            }
            return static_cast<uint8>(EAthenaGamePhaseStep::GetReady);

        case EAthenaGamePhase::Aircraft:
        {
            uint8 Step =
                static_cast<uint8>(EAthenaGamePhaseStep::BusFlying);
            if (!GameState->HasAircrafts())
                return Step;

            auto& Aircrafts = GameState->Aircrafts;
            if (!IsSaneArray(Aircrafts.Num(), Aircrafts.Max(), 16))
                return Step;

            for (auto Aircraft : Aircrafts)
            {
                if (!Aircraft || !Aircraft->HasDropStartTime())
                    continue;
                if (Aircraft->DropStartTime > Now)
                {
                    Step =
                        static_cast<uint8>(
                            EAthenaGamePhaseStep::BusLocked);
                    TimeRemaining = (std::max)(
                        TimeRemaining,
                        Aircraft->DropStartTime - Now + 1.0f);
                }
            }
            return Step;
        }

        case EAthenaGamePhase::SafeZones:
        {
            if (GameState->HasbIsInFinalCountdown() &&
                GameState->bIsInFinalCountdown)
            {
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::FinalCountdown);
            }
            if (GameState->HasbIsInCountdown() &&
                GameState->bIsInCountdown)
            {
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::Countdown);
            }

            auto Indicator =
                GameState->HasSafeZoneIndicator()
                    ? GameState->SafeZoneIndicator
                    : nullptr;
            const UClass* IndicatorClass =
                AFortSafeZoneIndicator::StaticClass();
            if (!Indicator || !IndicatorClass ||
                !Indicator->IsA(IndicatorClass))
            {
                if (GameState->HasSafeZonesStartTime() &&
                    GameState->SafeZonesStartTime > Now)
                {
                    TimeRemaining =
                        GameState->SafeZonesStartTime - Now;
                }
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::StormForming);
            }

            auto SafeZoneIndicator =
                static_cast<AFortSafeZoneIndicator*>(Indicator);
            if (!SafeZoneIndicator->HasSafeZoneStartShrinkTime() ||
                !SafeZoneIndicator->HasSafeZoneFinishShrinkTime())
            {
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::StormForming);
            }

            if (SafeZoneIndicator->SafeZoneStartShrinkTime > Now)
            {
                TimeRemaining =
                    SafeZoneIndicator->SafeZoneStartShrinkTime - Now;
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::StormHolding);
            }
            if (SafeZoneIndicator->SafeZoneFinishShrinkTime > Now)
            {
                TimeRemaining =
                    SafeZoneIndicator->SafeZoneFinishShrinkTime - Now;
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::StormShrinking);
            }
            return static_cast<uint8>(
                EAthenaGamePhaseStep::StormHolding);
        }

        case EAthenaGamePhase::EndGame:
            return static_cast<uint8>(EAthenaGamePhaseStep::EndGame);

        default:
            return static_cast<uint8>(EAthenaGamePhaseStep::None);
        }
    }

    void UpdateAndNotifyHeistGamePhaseStep(
        AFortGameStateAthena* GameState,
        float Now)
    {
        if (!GameState || !GameState->HasGamePhaseStep())
            return;

        float TimeRemaining = 0.0f;
        const uint8 NewStep =
            DeriveGamePhaseStep(GameState, Now, TimeRemaining);
        if (NewStep ==
            static_cast<uint8>(EAthenaGamePhaseStep::None))
        {
            return;
        }

        if (GameState->HasGamePhaseStepTimeRemaining())
            GameState->GamePhaseStepTimeRemaining = TimeRemaining;

        const bool bNeedsCompatibilityWrite =
            GameState->GamePhaseStep != NewStep;
        const bool bChangedByCompatibility =
            bNeedsCompatibilityWrite &&
            (!GHeistCompatibilityState
                 .bPublishedCompatibilityPhaseStep ||
             GHeistCompatibilityState
                 .PublishedCompatibilityPhaseStep != NewStep);
        if (bNeedsCompatibilityWrite)
        {
            const uint8 PreviousStep = GameState->GamePhaseStep;
            GameState->GetGamePhaseStep() = NewStep;
            if (bChangedByCompatibility)
            {
                GHeistCompatibilityState
                    .bPublishedCompatibilityPhaseStep = true;
                GHeistCompatibilityState
                    .PublishedCompatibilityPhaseStep = NewStep;
                GHeistCompatibilityState.bObservedNativePhaseStep =
                    false;
                GameState->ForceNetUpdate();
                SDK::DbgLog(
                    "[Heist] GamePhaseStep %u -> %u "
                    "(remaining=%.2f)\n",
                    static_cast<unsigned>(PreviousStep),
                    static_cast<unsigned>(NewStep),
                    TimeRemaining);
            }
        }
        else if (
            !GHeistCompatibilityState
                 .bPublishedCompatibilityPhaseStep ||
            GHeistCompatibilityState.PublishedCompatibilityPhaseStep !=
                NewStep)
        {
            // Native phase publication reached this step before the
            // compatibility tick. Trust its delegate path and do not send a
            // second callback to mutators which already existed. Remember
            // those mutators so one which appears later can still receive the
            // current step on a subsequent tick.
            GHeistCompatibilityState.bPublishedCompatibilityPhaseStep =
                false;
            GHeistCompatibilityState.PublishedCompatibilityPhaseStep =
                NewStep;
            if (!GHeistCompatibilityState.bObservedNativePhaseStep ||
                GHeistCompatibilityState.ObservedNativePhaseStep !=
                    NewStep)
            {
                GHeistCompatibilityState.bObservedNativePhaseStep = true;
                GHeistCompatibilityState.ObservedNativePhaseStep =
                    NewStep;

                auto NativeHeists = FindHeistMutators(GameState);
                std::unordered_set<AFortAthenaMutator_Heist*>
                    LiveNativeHeists;
                for (auto Heist : NativeHeists)
                {
                    if (!Heist)
                        continue;
                    LiveNativeHeists.insert(Heist);
                    GHeistCompatibilityState
                        .LastNotifiedPhaseSteps[Heist] = NewStep;
                }

                for (auto Iterator =
                         GHeistCompatibilityState
                             .LastNotifiedPhaseSteps.begin();
                     Iterator !=
                         GHeistCompatibilityState
                             .LastNotifiedPhaseSteps.end();)
                {
                    if (!LiveNativeHeists.contains(Iterator->first))
                    {
                        Iterator =
                            GHeistCompatibilityState
                                .LastNotifiedPhaseSteps.erase(Iterator);
                    }
                    else
                    {
                        ++Iterator;
                    }
                }
                return;
            }
        }

        std::unordered_set<UObject*> BroadcastTargets;
        if (bChangedByCompatibility)
        {
            BroadcastGamePhaseStepChanged(
                GameState, NewStep, BroadcastTargets);
        }

        auto Heists = FindHeistMutators(GameState);
        std::unordered_set<AFortAthenaMutator_Heist*> LiveHeists;
        for (auto Heist : Heists)
        {
            if (!Heist)
                continue;
            LiveHeists.insert(Heist);

            auto Existing =
                GHeistCompatibilityState.LastNotifiedPhaseSteps.find(
                    Heist);
            if (Existing !=
                    GHeistCompatibilityState.LastNotifiedPhaseSteps.end() &&
                Existing->second == NewStep)
            {
                continue;
            }

            UFunction* OnGamePhaseStepChanged =
                Heist->GetFunction("OnGamePhaseStepChanged");
            if (BroadcastTargets.contains(Heist))
            {
                GHeistCompatibilityState.LastNotifiedPhaseSteps[Heist] =
                    NewStep;
                continue;
            }

            if (!InvokeGamePhaseStep(
                    Heist, OnGamePhaseStepChanged, NewStep))
                continue;

            GHeistCompatibilityState.LastNotifiedPhaseSteps[Heist] =
                NewStep;
            SDK::DbgLog(
                "[Heist] notified %s of GamePhaseStep=%u\n",
                Heist->Name.ToString().c_str(),
                static_cast<unsigned>(NewStep));
        }

        for (auto Iterator =
                 GHeistCompatibilityState.LastNotifiedPhaseSteps.begin();
             Iterator !=
                 GHeistCompatibilityState.LastNotifiedPhaseSteps.end();)
        {
            if (!LiveHeists.contains(Iterator->first))
                Iterator =
                    GHeistCompatibilityState.LastNotifiedPhaseSteps.erase(
                        Iterator);
            else
                ++Iterator;
        }
    }

    bool IsSpawnerAlreadyResolved(
        AFortAthenaExitCraftSpawner* Spawner,
        const std::vector<AFortAthenaMutator_Heist*>& Heists)
    {
        const UStruct* EntryStruct =
            FHeistExitCraftData::StaticStruct();
        const UClass* CraftClass =
            AFortAthenaExitCraft::StaticClass();
        if (!IsLiveObject(Spawner) || !EntryStruct ||
            !CraftClass ||
            !FHeistExitCraftData::HasExitCraftSpawner() ||
            !FHeistExitCraftData::HasSpawnedExitCraft())
        {
            return false;
        }

        const int32 EntrySize = EntryStruct->GetPropertiesSize();
        if (EntrySize <= 0 || EntrySize > 0x100)
            return false;

        for (auto Heist : Heists)
        {
            if (!Heist || !Heist->HasSpawnedExitCraftList())
                continue;

            auto& Entries = Heist->SpawnedExitCraftList;
            if (!IsSaneArray(Entries.Num(), Entries.Max(), 16))
                continue;
            for (int32 Index = 0; Index < Entries.Num(); ++Index)
            {
                auto& Entry = Entries.Get(Index, EntrySize);
                if (Entry.ExitCraftSpawner == Spawner &&
                    IsLiveObject(Entry.SpawnedExitCraft) &&
                    Entry.SpawnedExitCraft->IsA(CraftClass))
                {
                    return true;
                }
            }
        }
        return false;
    }

    struct FReflectedPropertySpan
    {
        int32 Offset = -1;
        uint32 Size = 0;
    };

    bool ResolveReflectedPropertySpan(
        const UStruct* Owner,
        const char* Name,
        uint32 ExpectedSize,
        FReflectedPropertySpan& OutSpan)
    {
        OutSpan = {};
        if (!Owner || !Name)
            return false;

        const UField* Property = Owner->GetProperty(Name);
        if (!Property ||
            !SDK::MemReadable(Property, sizeof(UField)) ||
            Offsets::ElementSize == 0)
        {
            return false;
        }

        const uint32 Offset = Owner->GetOffset(Name);
        const uint32 Size =
            GetFromOffset<uint32>(
                Property, Offsets::ElementSize);
        const int32 OwnerSize = Owner->GetPropertiesSize();
        if (Offset == uint32(-1) || Size == 0 ||
            (ExpectedSize != 0 && Size != ExpectedSize) ||
            OwnerSize <= 0 ||
            Offset > static_cast<uint32>(OwnerSize) ||
            Size >
                static_cast<uint32>(OwnerSize) - Offset)
        {
            return false;
        }

        OutSpan.Offset = static_cast<int32>(Offset);
        OutSpan.Size = Size;
        return true;
    }

    bool ReplaceExitCraftSpawnerMarker(
        AFortGameStateAthena* GameState,
        AFortAthenaExitCraftSpawner* Spawner,
        AFortAthenaExitCraft* Craft,
        UFortAthenaExitCraftInfo* Info)
    {
        if (!FFortAthenaNativeLTMCompatibility::
                IsSupportedBuild() ||
            !IsLiveObject(GameState) ||
            !IsLiveObject(Spawner) ||
            !IsLiveObject(Craft) ||
            !IsLiveObject(Info))
        {
            return false;
        }

        FReflectedPropertySpan SpecialActorDataSpan;
        if (!ResolveReflectedPropertySpan(
                GameState->Class,
                "SpecialActorData",
                sizeof(void*),
                SpecialActorDataSpan))
        {
            return false;
        }

        auto SpecialActorData =
            GetFromOffset<AActor*>(
                GameState,
                SpecialActorDataSpan.Offset);
        const UClass* SpecialActorDataClass =
            FindClass("FortSpecialActorReplicationInfo");
        if (!IsLiveObject(SpecialActorData) ||
            !SpecialActorDataClass ||
            !SpecialActorData->IsA(SpecialActorDataClass))
        {
            return false;
        }

        const UStruct* RepDataArrayStruct =
            FindStruct("SpecialActorRepDataArray");
        const UStruct* RepDataStruct =
            FindStruct("SpecialActorRepData");
        if (!RepDataArrayStruct || !RepDataStruct)
            return false;

        FReflectedPropertySpan RepListSpan;
        FReflectedPropertySpan SpecialActorListSpan;
        if (!ResolveReflectedPropertySpan(
                SpecialActorData->Class,
                "SpecialActorRepList",
                0,
                RepListSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataArrayStruct,
                "SpecialActorList",
                sizeof(TArray<uint8>),
                SpecialActorListSpan) ||
            RepListSpan.Size <
                sizeof(FFastArraySerializer) ||
            SpecialActorListSpan.Offset <
                static_cast<int32>(
                    sizeof(FFastArraySerializer)) ||
            static_cast<uint32>(
                SpecialActorListSpan.Offset) +
                    SpecialActorListSpan.Size >
                RepListSpan.Size)
        {
            return false;
        }

        const int32 EntrySize =
            RepDataStruct->GetPropertiesSize();
        if (EntrySize <
                static_cast<int32>(
                    sizeof(FFastArraySerializerItem)) ||
            EntrySize > 0x400)
        {
            return false;
        }

        FReflectedPropertySpan SpecialActorIDSpan;
        FReflectedPropertySpan CategoryTagSpan;
        FReflectedPropertySpan SpecialActorSpan;
        FReflectedPropertySpan CurrentLocationSpan;
        FReflectedPropertySpan CurrentYawSpan;
        FReflectedPropertySpan MainIconBrushSpan;
        FReflectedPropertySpan MinimapScaleSpan;
        FReflectedPropertySpan CompassIconBrushSpan;
        FReflectedPropertySpan CompassScaleSpan;
        FReflectedPropertySpan DrawCompassIconSpan;
        if (!ResolveReflectedPropertySpan(
                RepDataStruct,
                "SpecialActorID",
                sizeof(FName),
                SpecialActorIDSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "CategoryTag",
                sizeof(FName),
                CategoryTagSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "SpecialActor",
                sizeof(void*),
                SpecialActorSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "CurrentLocation",
                FVector::Size(),
                CurrentLocationSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "CurrentYaw",
                sizeof(float),
                CurrentYawSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "MainIconBrush",
                0,
                MainIconBrushSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "MinimapScale",
                0,
                MinimapScaleSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "CompassIconBrush",
                0,
                CompassIconBrushSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "CompassScale",
                0,
                CompassScaleSpan) ||
            !ResolveReflectedPropertySpan(
                RepDataStruct,
                "bDrawCompassIcon",
                sizeof(bool),
                DrawCompassIconSpan))
        {
            return false;
        }

        FReflectedPropertySpan CraftTagSpan;
        FReflectedPropertySpan CraftMinimapBrushSpan;
        FReflectedPropertySpan CraftMinimapScaleSpan;
        FReflectedPropertySpan CraftCompassBrushSpan;
        FReflectedPropertySpan CraftCompassScaleSpan;
        FReflectedPropertySpan SpawnerIDSpan;
        FReflectedPropertySpan CraftIDSpan;
        if (!ResolveReflectedPropertySpan(
                Info->Class,
                "SpecialActorCraftTag",
                CategoryTagSpan.Size,
                CraftTagSpan) ||
            !ResolveReflectedPropertySpan(
                Info->Class,
                "CraftMinimapIconBrush",
                MainIconBrushSpan.Size,
                CraftMinimapBrushSpan) ||
            !ResolveReflectedPropertySpan(
                Info->Class,
                "CraftMinimapIconScale",
                MinimapScaleSpan.Size,
                CraftMinimapScaleSpan) ||
            !ResolveReflectedPropertySpan(
                Info->Class,
                "CraftCompassIconBrush",
                CompassIconBrushSpan.Size,
                CraftCompassBrushSpan) ||
            !ResolveReflectedPropertySpan(
                Info->Class,
                "CraftCompassIconScale",
                CompassScaleSpan.Size,
                CraftCompassScaleSpan) ||
            !ResolveReflectedPropertySpan(
                Spawner->Class,
                "SpawnerSpecialActorID",
                sizeof(FName),
                SpawnerIDSpan) ||
            !ResolveReflectedPropertySpan(
                Craft->Class,
                "CraftSpecialActorID",
                sizeof(FName),
                CraftIDSpan))
        {
            return false;
        }

        auto* RepListMemory =
            reinterpret_cast<uint8*>(SpecialActorData) +
            RepListSpan.Offset;
        auto& Serializer =
            *reinterpret_cast<FFastArraySerializer*>(
                RepListMemory);
        auto& SpecialActorList =
            *reinterpret_cast<TArray<uint8>*>(
                RepListMemory +
                SpecialActorListSpan.Offset);
        if (!IsSaneArray(
                SpecialActorList.Num(),
                SpecialActorList.Max(),
                512) ||
            !IsReadableArrayStorage(
                SpecialActorList.GetData(),
                SpecialActorList.Num(),
                static_cast<size_t>(EntrySize)))
        {
            return false;
        }

        FName SpawnerID;
        memcpy(
            &SpawnerID,
            reinterpret_cast<uint8*>(Spawner) +
                SpawnerIDSpan.Offset,
            sizeof(SpawnerID));

        uint8* Marker = nullptr;
        for (int32 Index = 0;
             Index < SpecialActorList.Num();
             ++Index)
        {
            auto* Candidate =
                &SpecialActorList.Get(Index, EntrySize);
            AActor* CandidateActor = nullptr;
            FName CandidateID;
            memcpy(
                &CandidateActor,
                Candidate + SpecialActorSpan.Offset,
                sizeof(CandidateActor));
            memcpy(
                &CandidateID,
                Candidate + SpecialActorIDSpan.Offset,
                sizeof(CandidateID));
            if (CandidateActor == Spawner ||
                (SpawnerID.IsValid() &&
                 CandidateID == SpawnerID))
            {
                Marker = Candidate;
                break;
            }
        }
        if (!Marker)
            return false;

        FName CraftCategory;
        memcpy(
            &CraftCategory,
            reinterpret_cast<uint8*>(Info) +
                CraftTagSpan.Offset,
            sizeof(CraftCategory));
        if (!CraftCategory.IsValid())
            return false;

        const auto CategoryString =
            CraftCategory.ToWString();
        const auto CraftNameString =
            Craft->Name.ToWString();
        if (CategoryString.empty() ||
            CraftNameString.empty())
        {
            return false;
        }
        const std::wstring CraftIDString =
            std::wstring(CategoryString.c_str()) +
            L"_" + CraftNameString.c_str();
        FName CraftID(CraftIDString.c_str());
        if (!CraftID.IsValid())
            return false;

        const FVector CraftLocation =
            Craft->K2_GetActorLocation();
        const FRotator CraftRotation =
            Craft->K2_GetActorRotation();
        const float CraftYaw =
            static_cast<float>(CraftRotation.Yaw);
        const bool bDrawCompassIcon = true;
        AActor* CraftActor = Craft;

        memcpy(
            Marker + SpecialActorIDSpan.Offset,
            &CraftID,
            sizeof(CraftID));
        memcpy(
            Marker + CategoryTagSpan.Offset,
            reinterpret_cast<uint8*>(Info) +
                CraftTagSpan.Offset,
            CategoryTagSpan.Size);
        memcpy(
            Marker + SpecialActorSpan.Offset,
            &CraftActor,
            sizeof(CraftActor));
        memcpy(
            Marker + CurrentLocationSpan.Offset,
            &CraftLocation,
            CurrentLocationSpan.Size);
        memcpy(
            Marker + CurrentYawSpan.Offset,
            &CraftYaw,
            sizeof(CraftYaw));
        memcpy(
            Marker + MainIconBrushSpan.Offset,
            reinterpret_cast<uint8*>(Info) +
                CraftMinimapBrushSpan.Offset,
            MainIconBrushSpan.Size);
        memcpy(
            Marker + MinimapScaleSpan.Offset,
            reinterpret_cast<uint8*>(Info) +
                CraftMinimapScaleSpan.Offset,
            MinimapScaleSpan.Size);
        memcpy(
            Marker + CompassIconBrushSpan.Offset,
            reinterpret_cast<uint8*>(Info) +
                CraftCompassBrushSpan.Offset,
            CompassIconBrushSpan.Size);
        memcpy(
            Marker + CompassScaleSpan.Offset,
            reinterpret_cast<uint8*>(Info) +
                CraftCompassScaleSpan.Offset,
            CompassScaleSpan.Size);
        memcpy(
            Marker + DrawCompassIconSpan.Offset,
            &bDrawCompassIcon,
            sizeof(bDrawCompassIcon));

        // Keep the server-only movement cache coherent with the marker's
        // initial replicated location when those 10.40 fields are present.
        FReflectedPropertySpan LastRepLocationSpan;
        if (ResolveReflectedPropertySpan(
                RepDataStruct,
                "LastRepLocation",
                FVector::Size(),
                LastRepLocationSpan))
        {
            memcpy(
                Marker + LastRepLocationSpan.Offset,
                &CraftLocation,
                LastRepLocationSpan.Size);
        }
        FReflectedPropertySpan LastRepYawSpan;
        if (ResolveReflectedPropertySpan(
                RepDataStruct,
                "LastRepYaw",
                sizeof(float),
                LastRepYawSpan))
        {
            memcpy(
                Marker + LastRepYawSpan.Offset,
                &CraftYaw,
                sizeof(CraftYaw));
        }

        memcpy(
            reinterpret_cast<uint8*>(Craft) +
                CraftIDSpan.Offset,
            &CraftID,
            sizeof(CraftID));
        Serializer.MarkItemDirty(
            *reinterpret_cast<FFastArraySerializerItem*>(
                Marker));
        SpecialActorData->ForceNetUpdate();
        SDK::DbgLog(
            "[Getaway1040] replaced spawner marker %s with "
            "physical craft marker %s\n",
            SpawnerID.ToString().c_str(),
            CraftID.ToString().c_str());
        return true;
    }

    bool SpawnExitCraft(
        AFortAthenaExitCraftSpawner* Spawner,
        AFortGameStateAthena* GameState)
    {
        if (!Spawner || !GameState ||
            !Spawner->HasExitCraftInfo())
        {
            SDK::DbgLog(
                "[Getaway1040] physical craft spawn rejected: "
                "spawner=%p gameState=%p hasInfo=%d\n",
                static_cast<void*>(Spawner),
                static_cast<void*>(GameState),
                Spawner && Spawner->HasExitCraftInfo() ? 1 : 0);
            return false;
        }

        auto Heists = FindHeistMutators(GameState);
        if (Heists.empty())
        {
            SDK::DbgLog(
                "[Getaway1040] physical craft spawn rejected for "
                "%s: no live Heist mutator\n",
                Spawner->Name.ToString().c_str());
            return false;
        }
        if (IsSpawnerAlreadyResolved(Spawner, Heists))
        {
            Spawner->K2_DestroyActor();
            return true;
        }

        UFortAthenaExitCraftInfo* Info = Spawner->ExitCraftInfo;
        if (!Info || !Info->HasExitCaftClass())
        {
            SDK::DbgLog(
                "[Getaway1040] physical craft spawn rejected for "
                "%s: missing ExitCraftInfo/class property\n",
                Spawner->Name.ToString().c_str());
            return false;
        }

        const UClass* CraftClass = Info->ExitCaftClass.Get();
        const UClass* ReflectedCraftClass =
            AFortAthenaExitCraft::StaticClass();
        if (!CraftClass || !ReflectedCraftClass)
        {
            SDK::DbgLog(
                "[Getaway1040] physical craft spawn rejected for "
                "%s: craftClass=%p reflectedClass=%p\n",
                Spawner->Name.ToString().c_str(),
                static_cast<const void*>(CraftClass),
                static_cast<const void*>(ReflectedCraftClass));
            return false;
        }
        FVector SpawnLocation = Spawner->K2_GetActorLocation();
        const FRotator SpawnRotation = Spawner->K2_GetActorRotation();

        const UStruct* ExitCraftInfoStruct =
            FExitCraftInfo::StaticStruct();
        if (Info->HasExitCraftInfo() && ExitCraftInfoStruct &&
            FExitCraftInfo::HasExitCraftZOffset())
        {
            SpawnLocation.Z +=
                Info->ExitCraftInfo.ExitCraftZOffset.Evaluate(0.0f);
        }

        UWorld* World = UWorld::GetWorld();
        if (!World)
        {
            SDK::DbgLog(
                "[Getaway1040] physical craft spawn rejected for "
                "%s: no world\n",
                Spawner->Name.ToString().c_str());
            return false;
        }

        auto Craft = World->SpawnActorUnfinished<AFortAthenaExitCraft>(
            CraftClass, SpawnLocation, SpawnRotation);
        if (!Craft)
        {
            SDK::DbgLog(
                "[Getaway1040] deferred physical craft spawn failed "
                "for %s class=%s\n",
                Spawner->Name.ToString().c_str(),
                CraftClass->Name.ToString().c_str());
            return false;
        }

        if (Craft->HasExitCraftInfo())
            Craft->ExitCraftInfo = Info;

        auto FinishedCraft =
            World->FinishSpawnActor<AFortAthenaExitCraft>(
                Craft, SpawnLocation, SpawnRotation);
        if (FinishedCraft)
        {
            Craft = FinishedCraft;
        }
        else if (!IsLiveObject(Craft))
        {
            SDK::DbgLog(
                "[Getaway1040] FinishSpawnActor failed for %s\n",
                Spawner->Name.ToString().c_str());
            return false;
        }
        else
        {
            // Some stripped 10.40 FinishSpawning thunks omit the reflected
            // return even though they finish the deferred actor in place.
            SDK::DbgLog(
                "[Getaway1040] FinishSpawnActor returned null for "
                "%s; retaining the live deferred craft\n",
                Spawner->Name.ToString().c_str());
        }

        if (FFortAthenaNativeLTMCompatibility::
                IsSupportedBuild() &&
            !ReplaceExitCraftSpawnerMarker(
                GameState,
                Spawner,
                Craft,
                Info))
        {
            SDK::DbgLog(
                "[Getaway1040] physical craft %s spawned, but "
                "its selected spawner marker was unavailable\n",
                Craft->Name.ToString().c_str());
        }

        bool bNotified = false;
        const UStruct* EntryStruct =
            FHeistExitCraftData::StaticStruct();
        const int32 EntrySize =
            EntryStruct ? EntryStruct->GetPropertiesSize() : 0;
        const bool bCanUpdateEntry =
            EntrySize > 0 && EntrySize <= 0x100 &&
            FHeistExitCraftData::HasExitCraftSpawner() &&
            FHeistExitCraftData::HasSpawnedExitCraft();

        for (auto Heist : Heists)
        {
            if (!Heist)
                continue;

            if (bCanUpdateEntry &&
                Heist->HasSpawnedExitCraftList())
            {
                auto& Entries = Heist->SpawnedExitCraftList;
                if (IsSaneArray(Entries.Num(), Entries.Max(), 16))
                {
                    for (int32 Index = 0;
                         Index < Entries.Num(); ++Index)
                    {
                        auto& Entry =
                            Entries.Get(Index, EntrySize);
                        const bool bHasValidExistingCraft =
                            IsLiveObject(
                                Entry.SpawnedExitCraft) &&
                            ReflectedCraftClass &&
                            Entry.SpawnedExitCraft->IsA(
                                ReflectedCraftClass);
                        if (Entry.ExitCraftSpawner == Spawner &&
                            !bHasValidExistingCraft)
                        {
                            Entry.SpawnedExitCraft = Craft;
                            break;
                        }
                    }
                }
            }

            UFunction* OnExitCraftSpawned =
                Heist->GetFunction("OnExitCraftSpawned");
            const bool bInvokedSpawnCallback =
                InvokeExitCraftSpawned(
                    Heist,
                    OnExitCraftSpawned,
                    Craft,
                    Spawner);

            if (FFortAthenaNativeLTMCompatibility::
                    IsSupportedBuild())
            {
                uint8 EntryState = uint8(-1);
                uint8 CraftState = uint8(-1);
                bool bRepairedEntryState = false;
                bool bRepairedCraftState = false;

                // OnExitCraftSpawned owns these transitions. Repair only a
                // craft left at None or an entry left in its pre-spawn
                // None/Incoming state; never overwrite a later native state.
                if (bCanUpdateEntry &&
                    FHeistExitCraftData::HasExitCraftState() &&
                    Heist->HasSpawnedExitCraftList())
                {
                    auto& Entries = Heist->SpawnedExitCraftList;
                    if (IsSaneArray(
                            Entries.Num(), Entries.Max(), 16))
                    {
                        for (int32 Index = 0;
                             Index < Entries.Num();
                             ++Index)
                        {
                            auto& Entry =
                                Entries.Get(Index, EntrySize);
                            if (Entry.ExitCraftSpawner != Spawner)
                                continue;

                            EntryState = Entry.ExitCraftState;
                            if (EntryState ==
                                    HeistExitCraftStateNone ||
                                EntryState ==
                                    HeistExitCraftStateIncoming)
                            {
                                Entry.ExitCraftState =
                                    uint8(
                                        HeistExitCraftStateSpawned);
                                EntryState =
                                    HeistExitCraftStateSpawned;
                                bRepairedEntryState = true;
                            }
                            break;
                        }
                    }
                }

                if (Craft->HasCurrentState())
                {
                    CraftState = Craft->CurrentState;
                    if (CraftState ==
                        ExitCraftActorStateNone)
                    {
                        Craft->CurrentState =
                            uint8(
                                ExitCraftActorStateSpawned);
                        CraftState =
                            ExitCraftActorStateSpawned;
                        bRepairedCraftState = true;
                        const bool bRanNewState =
                            CallReflectedByteParam(
                                Craft,
                                "OnNewState",
                                ExitCraftActorStateSpawned);
                        const bool bRanOnRep =
                            CallReflectedNoParams(
                                Craft,
                                "OnRep_CurrentState");
                        Craft->ForceNetUpdate();
                        SDK::DbgLog(
                            "[Getaway1040] repaired physical craft "
                            "state transition newState=%d onRep=%d\n",
                            bRanNewState ? 1 : 0,
                            bRanOnRep ? 1 : 0);
                    }
                }

                SDK::DbgLog(
                    "[Getaway1040] exit-craft callback state "
                    "invoked=%d craft=%u entry=%u repairedCraft=%d "
                    "repairedEntry=%d unspawned=%d spawned=%d "
                    "departed=%d\n",
                    bInvokedSpawnCallback ? 1 : 0,
                    static_cast<unsigned>(CraftState),
                    static_cast<unsigned>(EntryState),
                    bRepairedCraftState ? 1 : 0,
                    bRepairedEntryState ? 1 : 0,
                    Heist->HasNumUnspawnedExitCrafts()
                        ? Heist->NumUnspawnedExitCrafts
                        : -1,
                    Heist->HasNumSpawnedExitCrafts()
                        ? Heist->NumSpawnedExitCrafts
                        : -1,
                    Heist->HasNumDepartedExitCrafts()
                        ? Heist->NumDepartedExitCrafts
                        : -1);
            }
            Heist->ForceNetUpdate();
            bNotified = bNotified || bInvokedSpawnCallback;
        }

        if (!bNotified)
        {
            // A stripped/missing callback must not erase a successfully
            // finished and replicated craft. Keep the actor visible and log
            // the missing native bookkeeping rather than destroying the only
            // usable van.
            SDK::DbgLog(
                "[Getaway1040] kept physical craft %s even though "
                "OnExitCraftSpawned could not be invoked\n",
                Craft->Name.ToString().c_str());
        }

        SDK::DbgLog(
            "[Heist] spawned exit craft %s from %s\n",
            Craft->Name.ToString().c_str(),
            Spawner->Name.ToString().c_str());
        Spawner->K2_DestroyActor();
        return true;
    }

    struct FNativeCodeRange
    {
        uintptr_t Start = 0;
        uintptr_t End = 0;
    };

    bool GetNativeImageTextRange(FNativeCodeRange& OutRange)
    {
        OutRange = {};
        const uintptr_t ImageBase =
            Memcury::PE::GetModuleBase();
        if (!ImageBase ||
            !SDK::MemReadable(
                reinterpret_cast<void*>(ImageBase),
                sizeof(IMAGE_DOS_HEADER)))
        {
            return false;
        }

        const auto DosHeader =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(
                ImageBase);
        if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE ||
            DosHeader->e_lfanew <= 0)
        {
            return false;
        }

        const uintptr_t NtAddress =
            ImageBase +
            static_cast<uintptr_t>(DosHeader->e_lfanew);
        if (!SDK::MemReadable(
                reinterpret_cast<void*>(NtAddress),
                sizeof(IMAGE_NT_HEADERS64)))
        {
            return false;
        }

        const auto NtHeaders =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                NtAddress);
        if (NtHeaders->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const auto Section =
            IMAGE_FIRST_SECTION(
                const_cast<IMAGE_NT_HEADERS64*>(NtHeaders));
        const uint16 SectionCount =
            NtHeaders->FileHeader.NumberOfSections;
        if (!Section ||
            SectionCount == 0 || SectionCount > 128 ||
            !SDK::MemReadable(
                Section,
                static_cast<size_t>(SectionCount) *
                    sizeof(IMAGE_SECTION_HEADER)))
        {
            return false;
        }

        for (uint16 Index = 0;
             Index < SectionCount;
             ++Index)
        {
            const auto& Candidate = Section[Index];
            if (std::memcmp(
                    Candidate.Name,
                    ".text",
                    sizeof(".text") - 1) != 0)
            {
                continue;
            }

            const uintptr_t Start =
                ImageBase + Candidate.VirtualAddress;
            const uintptr_t Size =
                (std::max<uintptr_t>)(
                    Candidate.Misc.VirtualSize,
                    Candidate.SizeOfRawData);
            if (!Start || !Size ||
                Start >
                    (std::numeric_limits<uintptr_t>::max)() -
                    Size)
            {
                return false;
            }

            OutRange = {Start, Start + Size};
            return OutRange.End > OutRange.Start;
        }
        return false;
    }

    bool IsAddressInCodeRange(
        uintptr_t Address,
        size_t Size,
        const FNativeCodeRange& Range)
    {
        return Address >= Range.Start &&
            Address < Range.End &&
            Size <= Range.End - Address &&
            SDK::MemReadable(
                reinterpret_cast<void*>(Address),
                Size);
    }

    bool GetRuntimeFunctionRange(
        uintptr_t Address,
        const FNativeCodeRange& TextRange,
        FNativeCodeRange& OutRange)
    {
        OutRange = {};
        if (!IsAddressInCodeRange(
                Address, 1, TextRange))
        {
            return false;
        }

        DWORD64 RuntimeImageBase = 0;
        PRUNTIME_FUNCTION RuntimeFunction =
            RtlLookupFunctionEntry(
                static_cast<DWORD64>(Address),
                &RuntimeImageBase,
                nullptr);
        if (!RuntimeFunction ||
            !RuntimeImageBase ||
            !SDK::MemReadable(
                RuntimeFunction,
                sizeof(*RuntimeFunction)))
        {
            return false;
        }

        const uintptr_t Start =
            static_cast<uintptr_t>(RuntimeImageBase) +
            RuntimeFunction->BeginAddress;
        const uintptr_t End =
            static_cast<uintptr_t>(RuntimeImageBase) +
            RuntimeFunction->EndAddress;
        if (Start > Address || End <= Address ||
            Start < TextRange.Start || End > TextRange.End)
        {
            return false;
        }

        OutRange = {Start, End};
        return true;
    }

    uintptr_t ResolveRelativeBranchTarget(
        uintptr_t Instruction,
        size_t InstructionSize,
        size_t DisplacementOffset)
    {
        if (!Instruction ||
            DisplacementOffset + sizeof(int32) >
                InstructionSize ||
            !SDK::MemReadable(
                reinterpret_cast<void*>(Instruction),
                InstructionSize))
        {
            return 0;
        }

        int32 Displacement = 0;
        std::memcpy(
            &Displacement,
            reinterpret_cast<void*>(
                Instruction + DisplacementOffset),
            sizeof(Displacement));
        const intptr_t Base =
            static_cast<intptr_t>(
                Instruction + InstructionSize);
        const intptr_t Target =
            Base + static_cast<intptr_t>(Displacement);
        return Target > 0
            ? static_cast<uintptr_t>(Target)
            : 0;
    }

    bool IsNativeEmptyVoidStub(
        uintptr_t Address,
        const FNativeCodeRange& TextRange)
    {
        uintptr_t Cursor = Address;
        for (int32 Hop = 0; Hop < 5; ++Hop)
        {
            if (!IsAddressInCodeRange(
                    Cursor, 1, TextRange))
            {
                return false;
            }

            // Ignore alignment bytes before a folded leaf.
            int32 Padding = 0;
            while (Padding < 4 &&
                   IsAddressInCodeRange(
                       Cursor, 1, TextRange) &&
                   (*reinterpret_cast<const uint8*>(Cursor) ==
                        0x90 ||
                    *reinterpret_cast<const uint8*>(Cursor) ==
                        0xCC))
            {
                ++Cursor;
                ++Padding;
            }
            if (!IsAddressInCodeRange(
                    Cursor, 1, TextRange))
            {
                return false;
            }

            const uint8 Opcode =
                *reinterpret_cast<const uint8*>(Cursor);
            if (Opcode == 0xC3)
                return true;
            // MSVC can encode an empty noexcept/guarded leaf as a transformed
            // return that restores RSP and jumps through the saved return
            // address instead of emitting C3 directly.
            static constexpr uint8 TransformedReturn[] = {
                0x48, 0x8D, 0x64, 0x24, 0x08,
                0xFF, 0x64, 0x24, 0xF8};
            if (IsAddressInCodeRange(
                    Cursor,
                    sizeof(TransformedReturn),
                    TextRange) &&
                std::memcmp(
                    reinterpret_cast<const void*>(Cursor),
                    TransformedReturn,
                    sizeof(TransformedReturn)) == 0)
            {
                return true;
            }
            if (Opcode == 0xC2 &&
                IsAddressInCodeRange(
                    Cursor, 3, TextRange))
            {
                return true;
            }
            if (Opcode == 0xE9 &&
                IsAddressInCodeRange(
                    Cursor, 5, TextRange))
            {
                Cursor = ResolveRelativeBranchTarget(
                    Cursor, 5, 1);
                continue;
            }
            if (Opcode == 0xEB &&
                IsAddressInCodeRange(
                    Cursor, 2, TextRange))
            {
                int8 Displacement = 0;
                std::memcpy(
                    &Displacement,
                    reinterpret_cast<void*>(Cursor + 1),
                    sizeof(Displacement));
                Cursor = static_cast<uintptr_t>(
                    static_cast<intptr_t>(Cursor + 2) +
                    static_cast<intptr_t>(Displacement));
                continue;
            }
            return false;
        }
        return false;
    }

    bool DecodeRipRelativeLeaTarget(
        uintptr_t Address,
        const FNativeCodeRange& TextRange,
        uintptr_t& OutTarget)
    {
        OutTarget = 0;
        if (!IsAddressInCodeRange(
                Address, 7, TextRange))
        {
            return false;
        }

        const auto Bytes =
            reinterpret_cast<const uint8*>(Address);
        const bool bRexW =
            (Bytes[0] & 0xF8) == 0x48 &&
            (Bytes[0] & 0x08) != 0;
        const bool bRipRelativeLea =
            Bytes[1] == 0x8D &&
            (Bytes[2] & 0xC7) == 0x05;
        if (!bRexW || !bRipRelativeLea)
            return false;

        OutTarget =
            ResolveRelativeBranchTarget(Address, 7, 3);
        return OutTarget != 0;
    }

    bool IsLeaBoundIntoObject(
        uintptr_t LeaAddress,
        const FNativeCodeRange& TextRange)
    {
        if (!IsAddressInCodeRange(
                LeaAddress, 7, TextRange))
        {
            return false;
        }

        const auto LeaBytes =
            reinterpret_cast<const uint8*>(LeaAddress);
        const int32 LeaRegister =
            ((LeaBytes[2] >> 3) & 7) +
            ((LeaBytes[0] & 0x04) ? 8 : 0);
        const uintptr_t SearchEnd =
            (std::min)(
                TextRange.End,
                LeaAddress + 7 + 0x20);
        for (uintptr_t Cursor = LeaAddress + 7;
             Cursor + 3 <= SearchEnd;
             ++Cursor)
        {
            const auto Bytes =
                reinterpret_cast<const uint8*>(Cursor);
            const bool bRexW =
                (Bytes[0] & 0xF8) == 0x48 &&
                (Bytes[0] & 0x08) != 0;
            if (!bRexW || Bytes[1] != 0x89)
                continue;

            const int32 Mod = (Bytes[2] >> 6) & 3;
            const int32 SourceRegister =
                ((Bytes[2] >> 3) & 7) +
                ((Bytes[0] & 0x04) ? 8 : 0);
            const int32 Rm = Bytes[2] & 7;
            if (SourceRegister == LeaRegister &&
                (Mod == 1 || Mod == 2) &&
                Rm != 4)
            {
                return true;
            }
        }
        return false;
    }

    uintptr_t FindBoundEmptyTimerStubLea(
        uintptr_t Function,
        const FNativeCodeRange& TextRange,
        int32 Depth,
        std::unordered_set<uintptr_t>& Visited,
        uintptr_t& OutStub)
    {
        if (!Function || Depth < 0 ||
            Visited.contains(Function))
        {
            return 0;
        }
        Visited.insert(Function);

        FNativeCodeRange FunctionRange;
        if (!GetRuntimeFunctionRange(
                Function, TextRange, FunctionRange))
        {
            return 0;
        }

        const uintptr_t ScanStart =
            (std::max)(Function, FunctionRange.Start);
        const uintptr_t ScanEnd =
            (std::min)(
                FunctionRange.End,
                ScanStart + 0x2000);
        for (uintptr_t Cursor = ScanStart;
             Cursor + 7 <= ScanEnd;
             ++Cursor)
        {
            uintptr_t Target = 0;
            if (DecodeRipRelativeLeaTarget(
                    Cursor, TextRange, Target) &&
                IsNativeEmptyVoidStub(
                    Target, TextRange) &&
                IsLeaBoundIntoObject(
                    Cursor, TextRange))
            {
                OutStub = Target;
                return Cursor;
            }
        }

        if (Depth == 0)
            return 0;

        for (uintptr_t Cursor = ScanStart;
             Cursor + 5 <= ScanEnd;
             ++Cursor)
        {
            if (*reinterpret_cast<const uint8*>(Cursor) != 0xE8)
                continue;
            const uintptr_t Callee =
                ResolveRelativeBranchTarget(
                    Cursor, 5, 1);
            if (!IsAddressInCodeRange(
                    Callee, 1, TextRange))
            {
                continue;
            }
            if (const uintptr_t Result =
                    FindBoundEmptyTimerStubLea(
                        Callee,
                        TextRange,
                        Depth - 1,
                        Visited,
                        OutStub))
            {
                return Result;
            }
        }
        return 0;
    }

    bool PatchBoundTimerCallback(
        uintptr_t LeaAddress,
        uintptr_t ExpectedStub,
        const FNativeCodeRange& TextRange,
        void* Callback)
    {
        uintptr_t CurrentTarget = 0;
        if (!LeaAddress || !ExpectedStub || !Callback ||
            !DecodeRipRelativeLeaTarget(
                LeaAddress,
                TextRange,
                CurrentTarget) ||
            CurrentTarget != ExpectedStub ||
            !IsNativeEmptyVoidStub(
                CurrentTarget,
                TextRange) ||
            !IsLeaBoundIntoObject(
                LeaAddress,
                TextRange))
        {
            return false;
        }

        constexpr uintptr_t AllocationGranularity =
            0x10000;
        constexpr uintptr_t MaximumDistance =
            0x7FFF0000;
        const uintptr_t Base =
            LeaAddress &
            ~(AllocationGranularity - 1);
        void* Trampoline = nullptr;

        for (uintptr_t Distance = AllocationGranularity;
             Distance <= MaximumDistance;
             Distance += AllocationGranularity)
        {
            uintptr_t Candidates[2]{};
            Candidates[0] =
                Base >= Distance
                    ? Base - Distance
                    : 0;
            Candidates[1] =
                Base <=
                        (std::numeric_limits<uintptr_t>::max)() -
                            Distance
                    ? Base + Distance
                    : 0;

            for (const uintptr_t Candidate : Candidates)
            {
                if (!Candidate)
                    continue;
                const intptr_t Relative =
                    static_cast<intptr_t>(Candidate) -
                    static_cast<intptr_t>(LeaAddress + 7);
                if (Relative <
                        (std::numeric_limits<int32>::min)() ||
                    Relative >
                        (std::numeric_limits<int32>::max)())
                {
                    continue;
                }

                Trampoline = VirtualAlloc(
                    reinterpret_cast<void*>(Candidate),
                    0x1000,
                    MEM_COMMIT | MEM_RESERVE,
                    PAGE_EXECUTE_READWRITE);
                if (Trampoline)
                    break;
            }
            if (Trampoline)
                break;
        }
        if (!Trampoline)
            return false;

        uint8 Jump[14]{
            0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00};
        std::memcpy(
            Jump + 6,
            &Callback,
            sizeof(Callback));
        std::memcpy(
            Trampoline,
            Jump,
            sizeof(Jump));
        FlushInstructionCache(
            GetCurrentProcess(),
            Trampoline,
            sizeof(Jump));

        const intptr_t Relative64 =
            reinterpret_cast<intptr_t>(Trampoline) -
            static_cast<intptr_t>(LeaAddress + 7);
        if (Relative64 <
                (std::numeric_limits<int32>::min)() ||
            Relative64 >
                (std::numeric_limits<int32>::max)())
        {
            VirtualFree(Trampoline, 0, MEM_RELEASE);
            return false;
        }
        const int32 Relative =
            static_cast<int32>(Relative64);

        DWORD OldProtection = 0;
        void* DisplacementAddress =
            reinterpret_cast<void*>(LeaAddress + 3);
        if (!VirtualProtect(
                DisplacementAddress,
                sizeof(Relative),
                PAGE_EXECUTE_READWRITE,
                &OldProtection))
        {
            VirtualFree(Trampoline, 0, MEM_RELEASE);
            return false;
        }
        std::memcpy(
            DisplacementAddress,
            &Relative,
            sizeof(Relative));
        FlushInstructionCache(
            GetCurrentProcess(),
            reinterpret_cast<void*>(LeaAddress),
            7);
        DWORD IgnoredProtection = 0;
        VirtualProtect(
            DisplacementAddress,
            sizeof(Relative),
            OldProtection,
            &IgnoredProtection);
        return true;
    }

    bool IsSelectedNative1040ExitCraftSpawner(
        AFortGameStateAthena* GameState,
        AFortAthenaExitCraftSpawner* Spawner)
    {
        if (!IsLiveObject(GameState) ||
            !IsLiveObject(Spawner))
        {
            return false;
        }

        const UStruct* EntryStruct =
            FHeistExitCraftData::StaticStruct();
        const int32 EntrySize =
            EntryStruct
                ? EntryStruct->GetPropertiesSize()
                : 0;
        if (EntrySize <= 0 || EntrySize > 0x100 ||
            !FHeistExitCraftData::HasExitCraftSpawner() ||
            !FHeistExitCraftData::HasSpawnedExitCraft())
        {
            return false;
        }

        for (auto Heist : FindHeistMutators(GameState))
        {
            if (!IsLiveObject(Heist) ||
                !Heist->HasSpawnedExitCraftList())
            {
                continue;
            }
            auto& Entries = Heist->SpawnedExitCraftList;
            if (!IsSaneArray(
                    Entries.Num(), Entries.Max(), 64))
            {
                continue;
            }
            for (int32 Index = 0;
                 Index < Entries.Num();
                 ++Index)
            {
                auto& Entry =
                    Entries.Get(Index, EntrySize);
                if (Entry.ExitCraftSpawner == Spawner &&
                    !IsLiveObject(Entry.SpawnedExitCraft))
                {
                    return true;
                }
            }
        }
        return false;
    }

    void Native1040ExitCraftTimerCallback(
        AFortAthenaExitCraftSpawner* Spawner)
    {
        UWorld* World = UWorld::GetWorld();
        auto GameState =
            World && IsLiveObject(World->GameState)
                ? World->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        const UClass* SpawnerClass =
            AFortAthenaExitCraftSpawner::StaticClass();
        if (!IsLiveObject(GameState) ||
            !IsLiveObject(Spawner) ||
            !SpawnerClass ||
            !Spawner->IsA(SpawnerClass) ||
            !Spawner->HasExitCraftInfo() ||
            !IsLiveObject(Spawner->ExitCraftInfo) ||
            FindHeistMutators(GameState).empty())
        {
            SDK::DbgLog(
                "[Getaway1040] ignored exit-craft timer callback "
                "outside a live Heist match spawner=%p\n",
                static_cast<void*>(Spawner));
            return;
        }

        const bool bPresentInSelectedList =
            IsSelectedNative1040ExitCraftSpawner(
                GameState, Spawner);
        // This callback is installed at the bound completion inside this
        // exact spawner's StartExitCraftSpawnTimer implementation. Its type,
        // live Heist, and ExitCraftInfo checks above make the timer completion
        // authoritative even if SpawnedExitCraftList publishes one tick late.
        SDK::DbgLog(
            "[Getaway1040] native timer completed for selected "
            "%s listMatch=%d; materializing its physical craft\n",
            Spawner->Name.ToString().c_str(),
            bPresentInSelectedList ? 1 : 0);
        if (!SpawnExitCraft(Spawner, GameState))
        {
            SDK::DbgLog(
                "[Getaway1040] native timer callback could not "
                "materialize %s; watchdog remains active\n",
                Spawner->Name.ToString().c_str());
        }
    }

    bool InstallNative1040ExitCraftTimerPatch()
    {
        static bool bInstalled = false;
        static int32 AttemptCount = 0;
        if (bInstalled)
            return true;
        if (AttemptCount >= 3)
            return false;
        ++AttemptCount;

        const auto DefaultSpawner =
            AFortAthenaExitCraftSpawner::GetDefaultObj();
        UFunction* StartTimerFunction =
            DefaultSpawner
                ? DefaultSpawner->GetFunction(
                      "StartExitCraftSpawnTimer")
                : nullptr;
        if (!StartTimerFunction ||
            !StartTimerFunction->ExecFunction)
        {
            SDK::DbgLog(
                "[Getaway1040] timer callback patch unavailable: "
                "StartExitCraftSpawnTimer reflection missing\n");
            return false;
        }

        FNativeCodeRange TextRange;
        if (!GetNativeImageTextRange(TextRange))
        {
            SDK::DbgLog(
                "[Getaway1040] timer callback patch unavailable: "
                "executable .text range missing\n");
            return false;
        }

        const uintptr_t ExecFunction =
            reinterpret_cast<uintptr_t>(
                StartTimerFunction->ExecFunction);
        if (!IsAddressInCodeRange(
                ExecFunction, 5, TextRange))
        {
            SDK::DbgLog(
                "[Getaway1040] timer callback patch unavailable: "
                "reflected exec address is outside .text\n");
            return false;
        }
        uintptr_t Candidates[10]{};
        int32 CandidateCount = 0;
        Candidates[CandidateCount++] = ExecFunction;
        const uintptr_t ThunkScanEnd =
            (std::min)(
                TextRange.End,
                ExecFunction + 0x40);
        for (uintptr_t Cursor = ExecFunction;
             CandidateCount <
                     static_cast<int32>(
                         std::size(Candidates)) &&
             Cursor + 5 <= ThunkScanEnd;
             ++Cursor)
        {
            if (*reinterpret_cast<const uint8*>(Cursor) !=
                0xE9)
            {
                continue;
            }
            const uintptr_t Target =
                ResolveRelativeBranchTarget(
                    Cursor, 5, 1);
            if (!IsAddressInCodeRange(
                    Target, 1, TextRange))
            {
                continue;
            }
            bool bDuplicate = false;
            for (int32 Index = 0;
                 Index < CandidateCount;
                 ++Index)
            {
                bDuplicate =
                    bDuplicate ||
                    Candidates[Index] == Target;
            }
            if (!bDuplicate)
                Candidates[CandidateCount++] = Target;
        }

        for (int32 Index = 0;
             Index < CandidateCount;
             ++Index)
        {
            const uintptr_t Binder = Candidates[Index];
            std::unordered_set<uintptr_t> Visited;
            uintptr_t EmptyStub = 0;
            const uintptr_t LeaAddress =
                FindBoundEmptyTimerStubLea(
                    Binder,
                    TextRange,
                    2,
                    Visited,
                    EmptyStub);
            if (!LeaAddress || !EmptyStub)
                continue;

            bInstalled = PatchBoundTimerCallback(
                LeaAddress,
                EmptyStub,
                TextRange,
                reinterpret_cast<void*>(
                    &Native1040ExitCraftTimerCallback));
            if (!bInstalled)
                continue;

            const uintptr_t ImageBase =
                Memcury::PE::GetModuleBase();
            SDK::DbgLog(
                "[Getaway1040] patched selected exit-craft "
                "timer callback at +0x%llX "
                "(emptyStub=+0x%llX)\n",
                static_cast<unsigned long long>(
                    LeaAddress - ImageBase),
                static_cast<unsigned long long>(
                    EmptyStub - ImageBase));
            return true;
        }

        SDK::DbgLog(
            "[Getaway1040] timer callback LEA was not resolved; "
            "selected-entry watchdog will be used (attempt %d/3)\n",
            AttemptCount);
        return false;
    }

    void TickExitCraftSpawners(
        AFortGameStateAthena* GameState,
        double Now)
    {
        if (!GameState ||
            Now < GHeistCompatibilityState.NextExitCraftDiscoveryTime)
        {
            return;
        }
        GHeistCompatibilityState.NextExitCraftDiscoveryTime =
            Now + ExitCraftDiscoveryInterval;

        const UClass* SpawnerClass =
            AFortAthenaExitCraftSpawner::StaticClass();
        if (!SpawnerClass)
            return;

        TArray<AActor*> SpawnerActors;
        Utils::GetAll(SpawnerClass, SpawnerActors);
        if (!IsSaneArray(
                SpawnerActors.Num(), SpawnerActors.Max(), 64))
        {
            SpawnerActors.Free();
            return;
        }

        std::unordered_set<AFortAthenaExitCraftSpawner*> LiveSpawners;
        for (auto Actor : SpawnerActors)
        {
            if (!Actor || !Actor->IsA(SpawnerClass))
                continue;

            auto Spawner =
                static_cast<AFortAthenaExitCraftSpawner*>(Actor);
            LiveSpawners.insert(Spawner);

            auto Scheduled =
                GHeistCompatibilityState.ScheduledExitCraftSpawners.find(
                    Spawner);
            if (Scheduled ==
                GHeistCompatibilityState.ScheduledExitCraftSpawners.end())
            {
                if (!Spawner->HasExitCraftInfo() ||
                    !Spawner->ExitCraftInfo ||
                    !Spawner->ExitCraftInfo->HasExitCraftInfo() ||
                    !FExitCraftInfo::StaticStruct() ||
                    !FExitCraftInfo::HasExitCraftSpawnDelay())
                {
                    continue;
                }

                float Delay =
                    Spawner->ExitCraftInfo->ExitCraftInfo
                        .ExitCraftSpawnDelay.Evaluate(0.0f);
                if (!std::isfinite(Delay) ||
                    Delay < 0.0f || Delay > 3600.0f)
                {
                    SDK::DbgLog(
                        "[Heist] rejected exit-craft delay %.2f on %s\n",
                        Delay,
                        Spawner->Name.ToString().c_str());
                    continue;
                }

                // StartExitCraftSpawnTimer normally performs this cleanup
                // before binding its stripped callback. The polling fallback
                // replaces that timer path, so preserve the cleanup when the
                // reflected no-parameter event is available.
                CallReflectedNoParams(
                    Spawner, "DestroyBlockingActors");

                Scheduled =
                    GHeistCompatibilityState
                        .ScheduledExitCraftSpawners
                        .emplace(Spawner, Now + Delay).first;
                SDK::DbgLog(
                    "[Heist] scheduled %s in %.2f seconds\n",
                    Spawner->Name.ToString().c_str(), Delay);
            }

            if (Now >= Scheduled->second &&
                SpawnExitCraft(Spawner, GameState))
            {
                GHeistCompatibilityState
                    .ScheduledExitCraftSpawners.erase(Spawner);
            }
        }

        for (auto Iterator =
                 GHeistCompatibilityState
                     .ScheduledExitCraftSpawners.begin();
             Iterator !=
                 GHeistCompatibilityState
                     .ScheduledExitCraftSpawners.end();)
        {
            if (!LiveSpawners.contains(Iterator->first))
                Iterator =
                    GHeistCompatibilityState
                        .ScheduledExitCraftSpawners.erase(Iterator);
            else
                ++Iterator;
        }

        SpawnerActors.Free();
    }

    bool IsNative1040HeistStateUninitialized(
        AFortAthenaMutator_Heist* Heist)
    {
        if (!IsLiveObject(Heist) ||
            !Heist->HasSpawnedExitCraftList() ||
            !Heist->HasRemainingExitCraftSpawnIndexes() ||
            !Heist->HasNumUnspawnedExitCrafts() ||
            !Heist->HasNumSpawnedExitCrafts() ||
            !Heist->HasNumDepartedExitCrafts())
        {
            return false;
        }

        auto& SpawnedEntries = Heist->SpawnedExitCraftList;
        auto& RemainingIndexes =
            Heist->RemainingExitCraftSpawnIndexes;
        if (!IsSaneArray(
                SpawnedEntries.Num(),
                SpawnedEntries.Max(),
                64) ||
            !IsSaneArray(
                RemainingIndexes.Num(),
                RemainingIndexes.Max(),
                64) ||
            SpawnedEntries.Num() != 0 ||
            RemainingIndexes.Num() != 0 ||
            Heist->NumUnspawnedExitCrafts != 0 ||
            Heist->NumSpawnedExitCrafts != 0 ||
            Heist->NumDepartedExitCrafts != 0)
        {
            return false;
        }

        // Do not use a world-wide craft actor as proof for this mutator: an
        // actor belonging to a replaced/stale Heist instance can otherwise
        // suppress the new instance's Setup recovery.
        return true;
    }

    void TickNative1040HeistPhaseBridge(
        AFortGameStateAthena* GameState,
        double Now)
    {
        if (!IsLiveObject(GameState) ||
            !GameState->HasGamePhaseStep())
            return;

        const uint8 CurrentStep = GameState->GamePhaseStep;
        if (CurrentStep ==
                static_cast<uint8>(EAthenaGamePhaseStep::None) ||
            CurrentStep >=
                static_cast<uint8>(EAthenaGamePhaseStep::Count))
        {
            return;
        }

        auto Heists = FindHeistMutators(GameState);
        if (Heists.empty())
        {
            GHeistCompatibilityState
                .ObservedNative1040PhaseSteps.clear();
            GHeistCompatibilityState
                .CompletedNative1040SetupRecovery.clear();
            GHeistCompatibilityState
                .ManualNative1040PhaseInvocations.clear();
            GHeistCompatibilityState
                .Native1040SetupRecoveryAttempts.clear();
            GHeistCompatibilityState
                .Native1040NextSetupRecoveryAttemptTimes.clear();
            GHeistCompatibilityState
                .bLoggedNative1040SafeZoneUnavailable = false;
            return;
        }

        TScriptInterface<IInterface> SafeZoneInterface{};
        bool bTriedResolvingSafeZone = false;
        bool bHasSafeZone = false;
        auto EnsureSafeZone =
            [&]()
            {
                if (!bTriedResolvingSafeZone)
                {
                    bTriedResolvingSafeZone = true;
                    bHasSafeZone =
                        ResolveRelevantSafeZoneInterface(
                            GameState,
                            SafeZoneInterface);
                }
                return bHasSafeZone;
            };
        std::unordered_set<AFortAthenaMutator_Heist*> LiveHeists;
        for (auto Heist : Heists)
        {
            if (!IsLiveObject(Heist))
                continue;
            LiveHeists.insert(Heist);

            if (!GHeistCompatibilityState
                     .CompletedNative1040SetupRecovery
                     .contains(Heist))
            {
                if (!IsNative1040HeistStateUninitialized(
                        Heist))
                {
                    // Any populated transient selection/counter state proves
                    // native Setup already initialized this mutator.
                    GHeistCompatibilityState
                        .CompletedNative1040SetupRecovery
                        .insert(Heist);
                    GHeistCompatibilityState
                        .Native1040SetupRecoveryAttempts.erase(
                            Heist);
                    GHeistCompatibilityState
                        .Native1040NextSetupRecoveryAttemptTimes
                        .erase(Heist);
                }
                else
                {
                    // A manually registered mutator can first appear after
                    // Setup. The working 5.41 lifecycle initializes Heist at
                    // Setup before later StormHolding/Shrinking selections.
                    // Replay only that missed initializer. This exact,
                    // validated ProcessEvent is synchronous, while 10.40 has
                    // no reliable exposed post-Setup sentinel: valid Setup can
                    // leave the selection arrays empty until a later phase.
                    // Therefore one successful delivery is completion. Only a
                    // call that could not be made is retried, preventing
                    // duplicate delegates and candidate minimap markers.
                    int32& Attempts =
                        GHeistCompatibilityState
                            .Native1040SetupRecoveryAttempts[Heist];
                    double& NextAttemptTime =
                        GHeistCompatibilityState
                            .Native1040NextSetupRecoveryAttemptTimes[
                                Heist];
                    if (Attempts >=
                        Native1040SetupRecoveryMaxAttempts)
                    {
                        if (Now < NextAttemptTime)
                            continue;

                        SDK::DbgLog(
                            "[Getaway1040] Setup recovery remained "
                            "uninitialized on %s after %d attempts; "
                            "continuing with authoritative step=%u\n",
                            Heist->Name.ToString().c_str(),
                            Attempts,
                            static_cast<unsigned>(CurrentStep));
                        GHeistCompatibilityState
                            .CompletedNative1040SetupRecovery
                            .insert(Heist);
                        GHeistCompatibilityState
                            .Native1040SetupRecoveryAttempts.erase(
                                Heist);
                        GHeistCompatibilityState
                            .Native1040NextSetupRecoveryAttemptTimes
                            .erase(Heist);
                    }
                    else
                    {
                        if (Now < NextAttemptTime)
                            continue;

                        const uint8 SetupStep =
                            static_cast<uint8>(
                                EAthenaGamePhaseStep::Setup);
                        if (!EnsureSafeZone())
                        {
                            if (!GHeistCompatibilityState
                                     .bLoggedNative1040SafeZoneUnavailable)
                            {
                                GHeistCompatibilityState
                                    .bLoggedNative1040SafeZoneUnavailable =
                                    true;
                                SDK::DbgLog(
                                    "[Getaway1040] waiting for the "
                                    "relevant safe-zone interface "
                                    "before Setup recovery\n");
                            }
                            continue;
                        }

                        NextAttemptTime =
                            Now +
                            Native1040SetupRecoveryInterval;
                        ++Attempts;
                        const int32 AttemptNumber = Attempts;
                        const bool bInvoked =
                            InvokeNative1040HeistGamePhaseStep(
                                Heist,
                                SafeZoneInterface,
                                SetupStep);
                        if (bInvoked)
                        {
                            GHeistCompatibilityState
                                .ObservedNative1040PhaseSteps[Heist] =
                                SetupStep;
                            GHeistCompatibilityState
                                .CompletedNative1040SetupRecovery
                                .insert(Heist);
                            GHeistCompatibilityState
                                .Native1040SetupRecoveryAttempts.erase(
                                    Heist);
                            GHeistCompatibilityState
                                .Native1040NextSetupRecoveryAttemptTimes
                                .erase(Heist);
                            GHeistCompatibilityState
                                .bLoggedNative1040SafeZoneUnavailable =
                                false;
                        }
                        SDK::DbgLog(
                            "[Getaway1040] Setup recovery attempt "
                            "%d/%d on %s invoked=%d%s\n",
                            AttemptNumber,
                            Native1040SetupRecoveryMaxAttempts,
                            Heist->Name.ToString().c_str(),
                            bInvoked ? 1 : 0,
                            bInvoked
                                ? "; marked complete after one "
                                  "synchronous delivery"
                                : "");
                        if (!bInvoked)
                            continue;
                    }
                }
            }

            auto Observed =
                GHeistCompatibilityState
                    .ObservedNative1040PhaseSteps.find(Heist);
            if (Observed !=
                    GHeistCompatibilityState
                        .ObservedNative1040PhaseSteps.end() &&
                Observed->second == CurrentStep)
            {
                continue;
            }

            if (!EnsureSafeZone())
            {
                if (!GHeistCompatibilityState
                         .bLoggedNative1040SafeZoneUnavailable)
                {
                    GHeistCompatibilityState
                        .bLoggedNative1040SafeZoneUnavailable = true;
                    SDK::DbgLog(
                        "[Getaway1040] waiting for the relevant "
                        "safe-zone interface before replaying "
                        "GamePhaseStep=%u\n",
                        static_cast<unsigned>(CurrentStep));
                }
                continue;
            }

            if (!InvokeNative1040HeistGamePhaseStep(
                    Heist,
                    SafeZoneInterface,
                    CurrentStep))
            {
                continue;
            }

            GHeistCompatibilityState
                .ObservedNative1040PhaseSteps[Heist] =
                CurrentStep;
            GHeistCompatibilityState
                .bLoggedNative1040SafeZoneUnavailable = false;
            SDK::DbgLog(
                "[Getaway1040] replayed missed native phase "
                "callback on %s step=%u\n",
                Heist->Name.ToString().c_str(),
                static_cast<unsigned>(CurrentStep));
        }

        for (auto Iterator =
                 GHeistCompatibilityState
                     .ObservedNative1040PhaseSteps.begin();
             Iterator !=
                 GHeistCompatibilityState
                     .ObservedNative1040PhaseSteps.end();)
        {
            if (!LiveHeists.contains(Iterator->first))
            {
                Iterator =
                    GHeistCompatibilityState
                        .ObservedNative1040PhaseSteps.erase(
                            Iterator);
            }
            else
            {
                ++Iterator;
            }
        }

        for (auto Iterator =
                 GHeistCompatibilityState
                     .CompletedNative1040SetupRecovery.begin();
             Iterator !=
                 GHeistCompatibilityState
                     .CompletedNative1040SetupRecovery.end();)
        {
            if (!LiveHeists.contains(*Iterator))
            {
                Iterator =
                    GHeistCompatibilityState
                        .CompletedNative1040SetupRecovery.erase(
                            Iterator);
            }
            else
            {
                ++Iterator;
            }
        }

        for (auto Iterator =
                 GHeistCompatibilityState
                     .Native1040SetupRecoveryAttempts.begin();
             Iterator !=
                 GHeistCompatibilityState
                     .Native1040SetupRecoveryAttempts.end();)
        {
            if (!LiveHeists.contains(Iterator->first))
            {
                Iterator =
                    GHeistCompatibilityState
                        .Native1040SetupRecoveryAttempts.erase(
                            Iterator);
            }
            else
            {
                ++Iterator;
            }
        }

        for (auto Iterator =
                 GHeistCompatibilityState
                     .Native1040NextSetupRecoveryAttemptTimes
                     .begin();
             Iterator !=
                 GHeistCompatibilityState
                     .Native1040NextSetupRecoveryAttemptTimes
                     .end();)
        {
            if (!LiveHeists.contains(Iterator->first))
            {
                Iterator =
                    GHeistCompatibilityState
                        .Native1040NextSetupRecoveryAttemptTimes
                        .erase(Iterator);
            }
            else
            {
                ++Iterator;
            }
        }

        for (auto Iterator =
                 GHeistCompatibilityState
                     .ManualNative1040PhaseInvocations.begin();
             Iterator !=
                 GHeistCompatibilityState
                     .ManualNative1040PhaseInvocations.end();)
        {
            if (!LiveHeists.contains(*Iterator))
            {
                Iterator =
                    GHeistCompatibilityState
                        .ManualNative1040PhaseInvocations.erase(
                            Iterator);
            }
            else
            {
                ++Iterator;
            }
        }
    }

    AFortGameplayMutator* FindAuthoredNativeLTMMutator(
        AFortGameStateAthena* GameState,
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        if (!IsLiveObject(GameState) || !Descriptor)
            return nullptr;

        const UClass* ExpectedClass =
            FindClass(Descriptor->ExpectedMutatorBaseName);
        if (!ExpectedClass)
            return nullptr;

        if (GameState->HasGameplayMutators())
        {
            auto& Mutators = GameState->GameplayMutators;
            if (IsSaneArray(
                    Mutators.Num(), Mutators.Max(), 128) &&
                IsReadableArrayStorage(
                    Mutators.Data,
                    Mutators.Num(),
                    sizeof(AFortAthenaMutator*)))
            {
                for (auto Candidate : Mutators)
                {
                    if (IsLiveObject(Candidate) &&
                        Candidate->IsA(ExpectedClass))
                    {
                        return Candidate;
                    }
                }
            }
        }

        auto Candidate = FindWorldMutator(
            const_cast<UClass*>(ExpectedClass));
        return IsLiveObject(Candidate) &&
                Candidate->IsA(ExpectedClass)
            ? Candidate
            : nullptr;
    }

    template <typename DelegateType>
    bool EnsureAuthoredDelegateBinding(
        DelegateType& Delegate,
        UObject* Target,
        const wchar_t* FunctionName)
    {
        if (!IsLiveObject(Target) || !FunctionName ||
            FScriptDelegate::Size() != 0x10)
        {
            return false;
        }

        const FName ExpectedFunction(FunctionName);
        if (!Target->GetFunction(ExpectedFunction))
            return false;

        auto& InvocationList = Delegate.InvocationList;
        if (!IsSaneArray(
                InvocationList.Num(),
                InvocationList.Max(),
                128) ||
            !IsReadableArrayStorage(
                InvocationList.Data,
                InvocationList.Num(),
                static_cast<size_t>(
                    FScriptDelegate::Size())))
        {
            return false;
        }

        for (int32 Index = 0;
             Index < InvocationList.Num();
             ++Index)
        {
            auto& Existing =
                InvocationList.Get(
                    Index, FScriptDelegate::Size());
            if (Existing.Object.Get() == Target &&
                Existing.FunctionName == ExpectedFunction)
            {
                return true;
            }
        }

        Delegate.Bind(Target, ExpectedFunction);
        return true;
    }

    void EnsureAshtonGameplayDependenciesLoaded()
    {
        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        if (PhaseState.bAshtonDependenciesLoaded)
            return;

        UWorld* World = UWorld::GetWorld();
        const double Now =
            World
                ? UGameplayStatics::GetTimeSeconds(World)
                : 0.0;
        if (World &&
            Now <
                PhaseState
                    .NextAshtonDependencyPreloadTime)
        {
            return;
        }
        PhaseState.NextAshtonDependencyPreloadTime =
            Now + 1.0;

        auto LeaderAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"CarminePack/AS_CarminePack.AS_CarminePack");
        if (LeaderAbilitySet)
        {
            const_cast<UFortAbilitySet*>(LeaderAbilitySet)
                ->AddToRoot();
        }
        auto LeaderGadget =
            const_cast<UFortGadgetItemDefinition*>(
                FindObject<UFortGadgetItemDefinition>(
                    L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/AGID_CarminePack."
                    L"AGID_CarminePack"));
        if (LeaderGadget)
        {
            LeaderGadget->AddToRoot();
        }
        auto LeaderBacking =
            const_cast<UFortWeaponItemDefinition*>(
                FindObject<UFortWeaponItemDefinition>(
                    L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/D_CarminePack."
                    L"D_CarminePack"));
        if (LeaderBacking)
        {
            LeaderBacking->AddToRoot();
        }
        auto VillainAbilitySet =
            FindObject<UFortAbilitySet>(
                L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/AS_AshtonPack_Milo."
                L"AS_AshtonPack_Milo");
        if (VillainAbilitySet)
        {
            const_cast<UFortAbilitySet*>(
                VillainAbilitySet)->AddToRoot();
        }
        auto VillainGadget =
            FindObject<UFortGadgetItemDefinition>(
                L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/AGID_AshtonPack_Milo."
                L"AGID_AshtonPack_Milo");
        if (VillainGadget)
        {
            const_cast<UFortGadgetItemDefinition*>(
                VillainGadget)->AddToRoot();
        }

        static constexpr const wchar_t* ClassPaths[] = {
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_PassiveSetup."
                L"GA_CarminePack_PassiveSetup_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_DashOrSmash."
                L"GA_CarminePack_DashOrSmash_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_Jump_NotMoving."
                L"GA_CarminePack_Jump_NotMoving_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_Punch."
                L"GA_CarminePack_Punch_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_LifeSteal."
                L"GA_CarminePack_LifeSteal_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_GemPickup_Passive."
                L"GA_AshtonPack_Carmine_GemPickup_Passive_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_GemPickupFX."
                L"GA_AshtonPack_GemPickupFX_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_Health."
                L"GE_Carmine_Health_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_StartingShields."
                L"GE_Carmine_StartingShields_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_Health_Red."
                L"GE_Carmine_Health_Red_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_StartingShields_Red."
                L"GE_Carmine_StartingShields_Red_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/DA_CarminePack."
                L"DA_CarminePack_C",
            L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCN_Carmine_Beam."
                L"GCN_Carmine_Beam_C",
            L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCL_Carmine_Beam_Loop."
                L"GCL_Carmine_Beam_Loop_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Beam_Loop."
                L"GE_Carmine_GC_Beam_Loop_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_Beam_Damage."
                L"GE_Carmine_Beam_Damage_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_Beam_Damage_P."
                L"GE_Carmine_Beam_Damage_P_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_LockInPlace."
                L"GE_Ashton_Carmine_LockInPlace_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_AbilityBlocker."
                L"GE_Carmine_AbilityBlocker_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_DamageImmune."
                L"GE_Carmine_DamageImmune_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Aura."
                L"GE_Carmine_GC_Aura_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Skydive."
                L"GE_Carmine_GC_Skydive_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_GemPickUpAnim."
                L"GE_Ashton_Carmine_GemPickUpAnim_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_FinalGemPickUpAnim."
                L"GE_Ashton_Carmine_FinalGemPickUpAnim_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_StonePickUpAnim."
                L"GA_AshtonPack_Carmine_StonePickUpAnim_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_FinalGem."
                L"GA_AshtonPack_Carmine_FinalGem_C",
            L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCN_Carmine_Transform."
                L"GCN_Carmine_Transform_C",
            L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCL_Carmine_Skydive."
                L"GCL_Carmine_Skydive_C",
            L"/Game/Blueprints/Camera/Athena/"
                L"Athena_PlayerCameraModeCarmineSpawn."
                L"Athena_PlayerCameraModeCarmineSpawn_C",
            L"/Game/Blueprints/Camera/Athena/"
                L"Athena_PlayerCameraModeCarmine_Beam."
                L"Athena_PlayerCameraModeCarmine_Beam_C",
            L"/Game/Characters/Player/Male/Male_Avg_Base/"
                L"Gauntlet_Player_AnimBlueprint."
                L"Gauntlet_Player_AnimBlueprint_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_EquipWeapon_Milo."
                L"GA_AshtonPack_EquipWeapon_Milo_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_PassiveSetup_Milo."
                L"GA_AshtonPack_PassiveSetup_Milo_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_Milo_BlockAbilities."
                L"GA_AshtonPack_Milo_BlockAbilities_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GAT_AshtonPack_Milo_GemPickupHeal."
                L"GAT_AshtonPack_Milo_GemPickupHeal_C"
        };
        int32 LoadedClassCount = 0;
        for (const auto Path : ClassPaths)
        {
            auto Class = FindObject<UClass>(Path);
            if (!Class)
                continue;
            Class->AddToRoot();
            ++LoadedClassCount;
        }
        auto CarminePassiveClass =
            FindObject<UClass>(ClassPaths[0]);
        auto CarminePassiveDefault =
            CarminePassiveClass
                ? CarminePassiveClass->GetDefaultObj()
                : nullptr;
        const bool bCarmineTransitionHooksInstalled =
            CarminePassiveDefault &&
            EnsureCarminePassiveTransitionHooks(
                CarminePassiveDefault);

        static constexpr const wchar_t* VisualPaths[] = {
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast."
                L"P_Jim_LaserBlast",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Muzzle."
                L"P_Jim_LaserBlast_Muzzle",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Dust."
                L"P_Jim_LaserBlast_Dust",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Impact."
                L"P_Jim_LaserBlast_Impact",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Impact_Player."
                L"P_Jim_LaserBlast_Impact_Player",
            L"/Game/Animation/Game/MainPlayer/Skydive/Freefall/"
                L"Custom/Jim/Transitions/Spawn_Montage."
                L"Spawn_Montage",
            L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_FistBeam_Montage."
                L"Jim_FistBeam_Montage",
            L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_FistBeam_Outro_M."
                L"Jim_FistBeam_Outro_M",
            L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_PowerUp_Montage."
                L"Jim_PowerUp_Montage",
            L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_Victory_Montage."
                L"Jim_Victory_Montage"
        };
        int32 LoadedVisualCount = 0;
        for (const auto Path : VisualPaths)
        {
            auto Asset = FindObject<UObject>(Path);
            if (!Asset)
                continue;
            const_cast<UObject*>(Asset)->AddToRoot();
            ++LoadedVisualCount;
        }

        constexpr int32 ExpectedClassCount =
            static_cast<int32>(
                sizeof(ClassPaths) /
                sizeof(ClassPaths[0]));
        constexpr int32 ExpectedVisualCount =
            static_cast<int32>(
                sizeof(VisualPaths) /
                sizeof(VisualPaths[0]));
        PhaseState.bAshtonDependenciesLoaded =
            LeaderAbilitySet != nullptr &&
            LeaderGadget != nullptr &&
            LeaderBacking != nullptr &&
            LeaderGadget
                ->GetWeaponItemDefinition() != nullptr &&
            VillainAbilitySet != nullptr &&
            VillainGadget != nullptr &&
            LoadedClassCount == ExpectedClassCount &&
            LoadedVisualCount == ExpectedVisualCount &&
            bCarmineTransitionHooksInstalled;
        SDK::DbgLog(
            "[Ashton1040] dependency preload "
            "carmineSet=%p leaderGadget=%p backing=%p villainSet=%p "
            "villainGadget=%p transitionHooks=%d "
            "cue/abilityClasses=%d/%d "
            "visuals=%d/%d "
            "complete=%d\n",
            static_cast<const void*>(LeaderAbilitySet),
            static_cast<const void*>(LeaderGadget),
            static_cast<const void*>(
                LeaderGadget
                    ? LeaderGadget
                          ->GetWeaponItemDefinition()
                    : nullptr),
            static_cast<const void*>(VillainAbilitySet),
            static_cast<const void*>(VillainGadget),
            bCarmineTransitionHooksInstalled ? 1 : 0,
            LoadedClassCount,
            ExpectedClassCount,
            LoadedVisualCount,
            ExpectedVisualCount,
            PhaseState.bAshtonDependenciesLoaded
                ? 1
                : 0);
    }

    AFortAthenaMutator_InventoryOverride*
        FindAshtonInventoryOverride(
            AFortGameStateAthena* GameState)
    {
        const UClass* OverrideClass =
            AFortAthenaMutator_InventoryOverride::
                StaticClass();
        if (!IsLiveObject(GameState) ||
            !OverrideClass)
        {
            return nullptr;
        }

        if (GameState->HasGameplayMutators())
        {
            auto& Mutators = GameState->GameplayMutators;
            if (IsSaneArray(
                    Mutators.Num(), Mutators.Max(), 128) &&
                IsReadableArrayStorage(
                    Mutators.Data,
                    Mutators.Num(),
                    sizeof(AFortAthenaMutator*)))
            {
                for (auto Candidate : Mutators)
                {
                    if (IsLiveObject(Candidate) &&
                        Candidate->IsA(OverrideClass))
                    {
                        return static_cast<
                            AFortAthenaMutator_InventoryOverride*>(
                                Candidate);
                    }
                }
            }
        }

        // The authored actor can exist before GameplayMutators publishes it.
        // Villain inventory is needed on spawn island, so do not make that
        // replication/registration timing an inventory-grant prerequisite.
        auto Candidate = FindWorldMutator(
            const_cast<UClass*>(OverrideClass));
        if (IsLiveObject(Candidate) &&
            Candidate->IsA(OverrideClass))
        {
            return static_cast<
                AFortAthenaMutator_InventoryOverride*>(
                    Candidate);
        }
        return nullptr;
    }

    bool ResolveAshtonVillainLoadout(
        AFortGameStateAthena* GameState,
        AFortAthenaMutator_Ashton* Ashton)
    {
        auto& State = GAuthoredNativeLTMPhaseState;
        if (!IsLiveObject(GameState) ||
            !IsLiveObject(Ashton) ||
            !Ashton->HasVillainItemDefs() ||
            !FItemLoadoutContainer::StaticStruct() ||
            !FItemLoadoutTeamMap::StaticStruct() ||
            !FItemAndCount::StaticStruct() ||
            !FItemLoadoutContainer::HasLoadout() ||
            !FItemLoadoutTeamMap::HasTeamIndex() ||
            !FItemLoadoutTeamMap::HasLoadoutIndex() ||
            !FItemAndCount::HasCount() ||
            !FItemAndCount::HasItem())
        {
            return false;
        }

        State.AshtonMutator = Ashton;
        State.AshtonVillainGear.clear();
        auto& VillainDefinitions = Ashton->VillainItemDefs;
        if (!IsSaneArray(
                VillainDefinitions.Num(),
                VillainDefinitions.Max(),
                32) ||
            !IsReadableArrayStorage(
                VillainDefinitions.Data,
                VillainDefinitions.Num(),
                sizeof(UFortWorldItemDefinition*)))
        {
            return false;
        }
        for (auto Definition : VillainDefinitions)
        {
            if (IsLiveObject(Definition))
                State.AshtonVillainGear.insert(Definition);
        }
        if (Ashton->HasVillainLeaderItemDef() &&
            IsLiveObject(Ashton->VillainLeaderItemDef))
        {
            State.AshtonVillainGear.insert(
                Ashton->VillainLeaderItemDef);
        }

        auto Override =
            FindAshtonInventoryOverride(GameState);
        State.AshtonInventoryOverride = Override;
        if (!Override ||
            !Override->HasInventoryLoadouts() ||
            !Override->HasTeamLoadouts())
        {
            return false;
        }

        auto& Loadouts = Override->InventoryLoadouts;
        auto& TeamMaps = Override->TeamLoadouts;
        const int32 ContainerSize =
            FItemLoadoutContainer::Size();
        const int32 TeamMapSize =
            FItemLoadoutTeamMap::Size();
        const int32 ItemAndCountSize =
            FItemAndCount::Size();
        if (ContainerSize != 0x10 ||
            TeamMapSize != 0x4 ||
            ItemAndCountSize != 0x10 ||
            !IsSaneArray(
                Loadouts.Num(), Loadouts.Max(), 32) ||
            !IsSaneArray(
                TeamMaps.Num(), TeamMaps.Max(), 32) ||
            !IsReadableArrayStorage(
                Loadouts.Data,
                Loadouts.Num(),
                static_cast<size_t>(ContainerSize)) ||
            !IsReadableArrayStorage(
                TeamMaps.Data,
                TeamMaps.Num(),
                static_cast<size_t>(TeamMapSize)))
        {
            return false;
        }

        for (int32 TeamMapIndex = 0;
             TeamMapIndex < TeamMaps.Num();
             ++TeamMapIndex)
        {
            const auto& TeamMap =
                TeamMaps.Get(TeamMapIndex, TeamMapSize);
            if (TeamMap.TeamIndex == 0 ||
                TeamMap.TeamIndex == 255 ||
                TeamMap.LoadoutIndex >= Loadouts.Num())
            {
                continue;
            }

            const auto& Container =
                Loadouts.Get(
                    TeamMap.LoadoutIndex,
                    ContainerSize);
            const auto& Items = Container.Loadout;
            if (!IsSaneArray(
                    Items.Num(), Items.Max(), 64) ||
                !IsReadableArrayStorage(
                    Items.Data,
                    Items.Num(),
                    static_cast<size_t>(
                        ItemAndCountSize)))
            {
                continue;
            }

            bool bVillainLoadout = false;
            std::vector<std::pair<
                UFortItemDefinition*, int32>> Candidate;
            for (int32 ItemIndex = 0;
                 ItemIndex < Items.Num();
                 ++ItemIndex)
            {
                const auto& Entry =
                    Items.Get(
                        ItemIndex,
                        ItemAndCountSize);
                if (!IsLiveObject(Entry.Item) ||
                    Entry.Count <= 0 ||
                    Entry.Count > 1000)
                {
                    continue;
                }
                Candidate.emplace_back(
                    Entry.Item, Entry.Count);
                if (State.AshtonVillainGear.contains(
                        Entry.Item))
                {
                    bVillainLoadout = true;
                }
            }

            if (!bVillainLoadout || Candidate.empty())
                continue;

            State.AshtonVillainTeam =
                TeamMap.TeamIndex;
            State.AshtonVillainLoadout =
                std::move(Candidate);
            for (const auto& Entry :
                 State.AshtonVillainLoadout)
            {
                State.AshtonVillainGear.insert(
                    Entry.first);
            }
            State.bLoggedInvalidAshtonLoadout = false;
            SDK::DbgLog(
                "[Ashton1040] resolved authored villain "
                "loadout team=%u index=%u items=%llu "
                "gearDefinitions=%llu\n",
                static_cast<unsigned>(
                    State.AshtonVillainTeam),
                static_cast<unsigned>(
                    TeamMap.LoadoutIndex),
                static_cast<unsigned long long>(
                    State.AshtonVillainLoadout.size()),
                static_cast<unsigned long long>(
                    State.AshtonVillainGear.size()));
            return true;
        }

        return false;
    }

    bool ResolveExact1040AshtonVillainFallback(
        AFortGameStateAthena* GameState,
        AFortAthenaMutator_Ashton* Ashton)
    {
        if (VersionInfo.FortniteVersion != 10.40 ||
            !IsLiveObject(GameState) ||
            !IsLiveObject(Ashton) ||
            !IsNativeAshtonDescriptor(
                GNativeLTMCompatibilityState.Descriptor))
        {
            return false;
        }

        auto& State = GAuthoredNativeLTMPhaseState;
        State.AshtonMutator = Ashton;
        State.AshtonInventoryOverride =
            FindAshtonInventoryOverride(GameState);
        State.AshtonVillainTeam = 3;
        State.AshtonVillainLoadout.clear();
        State.AshtonVillainGear.clear();

        // Resolve the exact cooked definitions directly so an unpublished
        // InventoryOverride actor cannot leave the villain team empty on spawn
        // island. The authored team-3 loadout contains only the Milo gadget.
        // Its passive equips the primary ability weapon first, then grants the
        // launcher and jetpack with their definition-derived initialization.
        // Keep all four definitions in the allowlist, but never race that
        // passive by synthesizing its child rows here.
        static constexpr const wchar_t* GearPaths[] = {
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/AGID_JetPack_AshtonPack_Milo."
                L"AGID_JetPack_AshtonPack_Milo",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/AGID_AshtonPack_Milo."
                L"AGID_AshtonPack_Milo",
            L"/Game/Athena/Items/Weapons/"
                L"WID_AshtonPack_Milo.WID_AshtonPack_Milo",
            L"/Game/Athena/Items/Weapons/"
                L"WID_AshtonPack_Milo_Launcher."
                L"WID_AshtonPack_Milo_Launcher"
        };
        static constexpr const wchar_t* GearNames[] = {
            L"AGID_JetPack_AshtonPack_Milo",
            L"AGID_AshtonPack_Milo",
            L"WID_AshtonPack_Milo",
            L"WID_AshtonPack_Milo_Launcher"
        };

        UFortItemDefinition* ExactGear[
            sizeof(GearPaths) / sizeof(GearPaths[0])]{};
        for (size_t GearIndex = 0;
             GearIndex <
                 sizeof(GearPaths) / sizeof(GearPaths[0]);
             ++GearIndex)
        {
            auto Definition =
                const_cast<UFortWorldItemDefinition*>(
                    FindObject<UFortWorldItemDefinition>(
                        GearPaths[GearIndex]));
            if (!IsLiveObject(Definition))
                continue;

            Definition->AddToRoot();
            State.AshtonVillainGear.insert(
                Definition);
            ExactGear[GearIndex] = Definition;
        }

        // Prefer already-authored objects if a soft package dependency is
        // delayed, while retaining exact-name validation.
        if (Ashton->HasVillainItemDefs())
        {
            auto& VillainDefinitions =
                Ashton->VillainItemDefs;
            if (IsSaneArray(
                    VillainDefinitions.Num(),
                    VillainDefinitions.Max(),
                    32) &&
                IsReadableArrayStorage(
                    VillainDefinitions.Data,
                    VillainDefinitions.Num(),
                    sizeof(UFortWorldItemDefinition*)))
            {
                for (auto Definition :
                     VillainDefinitions)
                {
                    if (!IsLiveObject(Definition))
                        continue;
                    State.AshtonVillainGear.insert(
                        Definition);
                    const auto Name =
                        Definition->Name.ToWString();
                    for (size_t GearIndex = 0;
                         GearIndex <
                             sizeof(GearPaths) /
                                 sizeof(GearPaths[0]);
                        ++GearIndex)
                    {
                        if (!ExactGear[GearIndex] &&
                            Name == GearNames[GearIndex])
                        {
                            ExactGear[GearIndex] =
                                Definition;
                        }
                    }
                }
            }
        }

        // The complete authored gear contract must resolve. The cooked
        // passive creates the launcher and jetpack after a 0.1-second latent
        // step, but its primary is a transient ability weapon that can arrive
        // very late on a caught-up client. Replicate one stat-derived primary
        // row first, then the main gadget; the passive retains the authored
        // launcher/jetpack order without duplicating either child gadget.
        if (!IsLiveObject(ExactGear[0]) ||
            !IsLiveObject(ExactGear[1]) ||
            !IsLiveObject(ExactGear[2]) ||
            !IsLiveObject(ExactGear[3]))
        {
            return false;
        }

        State.AshtonVillainLoadout.emplace_back(
            ExactGear[2], 1);
        State.AshtonVillainLoadout.emplace_back(
            ExactGear[1], 1);

        auto ExactLeader =
            const_cast<UFortGadgetItemDefinition*>(
                FindObject<UFortGadgetItemDefinition>(
                    L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/AGID_CarminePack."
                    L"AGID_CarminePack"));
        if (!IsLiveObject(ExactLeader))
        {
            return false;
        }
        ExactLeader->AddToRoot();
        State.AshtonVillainGear.insert(ExactLeader);
        if (!State.bAshtonLeaderPolicyInitialized)
        {
            State.bAshtonLeaderPolicyInitialized = true;
            State.bAshtonLeaderVacant = true;
            if (Ashton->HasVillainLeaderPC())
                Ashton->VillainLeaderPC = nullptr;
        }
        Ashton->VillainLeaderItemDef = ExactLeader;
        Ashton->ForceNetUpdate();

        State.bLoggedInvalidAshtonLoadout = false;
        SDK::DbgLog(
            "[Ashton1040] using exact fallback villain "
            "loadout team=3 items=%llu leader=%s "
            "gearDefinitions=%llu override=%p\n",
            static_cast<unsigned long long>(
                State.AshtonVillainLoadout.size()),
            ExactLeader->Name.ToString().c_str(),
            static_cast<unsigned long long>(
                State.AshtonVillainGear.size()),
            static_cast<void*>(
                State.AshtonInventoryOverride));
        return true;
    }

    bool EnsureAshtonVillainConfiguration(
        AFortGameStateAthena* GameState)
    {
        auto& State = GAuthoredNativeLTMPhaseState;
        if (IsLiveObject(State.AshtonMutator) &&
            State.AshtonVillainTeam != 255 &&
            !State.AshtonVillainLoadout.empty() &&
            !State.AshtonVillainGear.empty())
        {
            return true;
        }

        const auto Descriptor =
            GNativeLTMCompatibilityState.Descriptor;
        auto Mutator =
            IsNativeAshtonDescriptor(Descriptor)
                ? FindAuthoredNativeLTMMutator(
                      GameState, Descriptor)
                : nullptr;
        const UClass* AshtonClass =
            AFortAthenaMutator_Ashton::StaticClass();
        auto Ashton =
            IsLiveObject(Mutator) &&
                    AshtonClass &&
                    Mutator->IsA(AshtonClass)
                ? static_cast<AFortAthenaMutator_Ashton*>(
                      Mutator)
                : nullptr;
        // Keep the exact 10.40 contract available even when the published
        // InventoryOverride is late. It preserves the authored one-row Milo
        // loadout and the mutator CDO's complete Carmine Thanos carrier.
        if (ResolveExact1040AshtonVillainFallback(
                GameState, Ashton))
        {
            return true;
        }
        if (ResolveAshtonVillainLoadout(
                GameState, Ashton))
        {
            return true;
        }

        if (!State.bLoggedInvalidAshtonLoadout)
        {
            State.bLoggedInvalidAshtonLoadout = true;
            SDK::DbgLog(
                "[Ashton1040] waiting for the authored "
                "team loadout and villain item definitions\n");
        }
        return false;
    }

    bool EnsureAshtonLeaderItem(
        AFortAthenaMutator_Ashton* Ashton,
        AFortPlayerControllerAthena* PlayerController,
        const char* Reason);

    bool IsValidAshtonLeaderController(
        AFortPlayerControllerAthena* PlayerController);

    int32 RemoveAllAshtonLeaderArtifacts(
        AFortAthenaMutator_Ashton* Ashton,
        AFortPlayerControllerAthena* PlayerController);

    const UFortItemDefinition*
        GetAshtonLeaderBackingDefinition(
            AFortAthenaMutator_Ashton* Ashton)
    {
        if (VersionInfo.FortniteVersion != 10.40 ||
            !IsLiveObject(Ashton) ||
            !Ashton->HasVillainLeaderItemDef() ||
            !IsLiveObject(
                Ashton->VillainLeaderItemDef))
        {
            return nullptr;
        }

        auto BackingDefinition =
            Ashton->VillainLeaderItemDef
                ->GetWeaponItemDefinition();
        if (!IsLiveObject(BackingDefinition))
            return nullptr;
        const auto BackingName =
            BackingDefinition->Name.ToWString();
        return BackingName == L"D_AshtonPack" ||
                BackingName == L"D_CarminePack"
            ? BackingDefinition
            : nullptr;
    }

    bool IsAshtonLeaderInventoryDefinition(
        AFortAthenaMutator_Ashton* Ashton,
        const UFortItemDefinition* ItemDefinition)
    {
        return IsLiveObject(ItemDefinition) &&
            IsLiveObject(Ashton) &&
            Ashton->HasVillainLeaderItemDef() &&
            (ItemDefinition ==
                 Ashton->VillainLeaderItemDef ||
             ItemDefinition ==
                 GetAshtonLeaderBackingDefinition(
                     Ashton));
    }

    bool IsAnyAshtonLeaderArtifact(
        AFortAthenaMutator_Ashton* Ashton,
        const UFortItemDefinition* ItemDefinition)
    {
        if (!IsLiveObject(ItemDefinition))
            return false;
        if (IsAshtonLeaderInventoryDefinition(
                Ashton, ItemDefinition))
        {
            return true;
        }
        const auto Name =
            ItemDefinition->Name.ToWString();
        return Name == L"AGID_AshtonPack" ||
            Name == L"D_AshtonPack" ||
            Name == L"AGID_CarminePack" ||
            Name == L"D_CarminePack";
    }

    const UFortItemDefinition* FindAshtonVillainGear(
        const wchar_t* DefinitionName)
    {
        if (!DefinitionName)
            return nullptr;
        for (auto Definition :
             GAuthoredNativeLTMPhaseState
                 .AshtonVillainGear)
        {
            if (IsLiveObject(Definition) &&
                Definition->Name.ToWString() ==
                    DefinitionName)
            {
                return Definition;
            }
        }
        return nullptr;
    }

    bool IsAshtonVillainUtilityDefinition(
        const UFortItemDefinition* ItemDefinition)
    {
        if (!IsLiveObject(ItemDefinition))
            return false;

        const uint8 ItemType =
            ItemDefinition->ItemType;
        return ItemType ==
                EFortItemType::GetWeaponHarvest() ||
            ItemType ==
                EFortItemType::GetBuildingPiece() ||
            ItemType ==
                EFortItemType::GetEditTool() ||
            ItemType ==
                EFortItemType::GetWorldResource();
    }

    const UFortItemDefinition*
        ResolveAshtonPlayerPickaxe(
            AFortPlayerControllerAthena*
                PlayerController)
    {
        if (!IsLiveObject(PlayerController))
            return nullptr;

        if (PlayerController
                ->HasCosmeticLoadoutPC() &&
            PlayerController
                ->CosmeticLoadoutPC.Pickaxe &&
            IsLiveObject(
                PlayerController
                    ->CosmeticLoadoutPC.Pickaxe
                    ->WeaponDefinition))
        {
            return PlayerController
                ->CosmeticLoadoutPC.Pickaxe
                ->WeaponDefinition;
        }
        if (PlayerController
                ->HasCustomizationLoadout() &&
            PlayerController
                ->CustomizationLoadout.Pickaxe &&
            IsLiveObject(
                PlayerController
                    ->CustomizationLoadout.Pickaxe
                    ->WeaponDefinition))
        {
            return PlayerController
                ->CustomizationLoadout.Pickaxe
                ->WeaponDefinition;
        }

        return FindObject<UFortItemDefinition>(
            L"/Game/Athena/Items/Weapons/"
            L"WID_Harvest_Pickaxe_Athena_C_T01."
            L"WID_Harvest_Pickaxe_Athena_C_T01");
    }

    bool EnsureAshtonVillainUtilityItems(
        AFortPlayerControllerAthena*
            PlayerController,
        const char* Reason)
    {
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(
                PlayerController->WorldInventory))
        {
            return false;
        }

        auto Inventory =
            PlayerController->WorldInventory;
        auto HasDefinition =
            [&](const UFortItemDefinition* Definition)
            {
                return IsLiveObject(Definition) &&
                    Inventory->Inventory
                            .ReplicatedEntries.Search(
                                [&](FFortItemEntry& Entry)
                                {
                                    return Entry
                                                .ItemDefinition ==
                                            Definition;
                                },
                                FFortItemEntry::Size()) !=
                        nullptr;
            };
        auto HasItemType =
            [&](uint8 ItemType)
            {
                return Inventory->Inventory
                           .ReplicatedEntries.Search(
                               [&](FFortItemEntry& Entry)
                               {
                                   return IsLiveObject(
                                              Entry
                                                  .ItemDefinition) &&
                                       Entry.ItemDefinition
                                               ->ItemType ==
                                           ItemType;
                               },
                               FFortItemEntry::Size()) !=
                    nullptr;
            };

        int32 Granted = 0;
        const auto Pickaxe =
            ResolveAshtonPlayerPickaxe(
                PlayerController);
        if (!HasItemType(
                EFortItemType::GetWeaponHarvest()) &&
            IsLiveObject(Pickaxe) &&
            Inventory->GiveItem(
                Pickaxe, 1, 0, 0,
                false, true))
        {
            ++Granted;
        }

        static constexpr const wchar_t*
            BuildingToolPaths[] = {
                L"/Game/Items/Weapons/BuildingTools/"
                L"BuildingItemData_Wall."
                L"BuildingItemData_Wall",
                L"/Game/Items/Weapons/BuildingTools/"
                L"BuildingItemData_Floor."
                L"BuildingItemData_Floor",
                L"/Game/Items/Weapons/BuildingTools/"
                L"BuildingItemData_Stair_W."
                L"BuildingItemData_Stair_W",
                L"/Game/Items/Weapons/BuildingTools/"
                L"BuildingItemData_RoofS."
                L"BuildingItemData_RoofS",
                L"/Game/Items/Weapons/BuildingTools/"
                L"EditTool.EditTool"
            };
        for (const auto Path : BuildingToolPaths)
        {
            auto Definition =
                FindObject<UFortItemDefinition>(
                    Path);
            if (!IsLiveObject(Definition) ||
                HasDefinition(Definition))
            {
                continue;
            }
            if (Inventory->GiveItem(
                    Definition, 1, 0, 0,
                    false, true))
            {
                ++Granted;
            }
        }

        if (Granted > 0)
        {
            Inventory->ForceNetUpdate();
            PlayerController->ForceNetUpdate();
            SDK::DbgLog(
                "[Ashton1040] restored Chitauri "
                "harvesting/build utility PC=%p "
                "items=%d reason=%s\n",
                static_cast<void*>(
                    PlayerController),
                Granted,
                Reason ? Reason : "unknown");
        }
        return Granted > 0;
    }

    bool FocusAshtonMiloPrimaryOnce(
        AFortPlayerControllerAthena*
            PlayerController)
    {
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(
                PlayerController->WorldInventory))
        {
            return false;
        }

        auto Entry =
            PlayerController->WorldInventory
                ->Inventory.ReplicatedEntries.Search(
                    [](FFortItemEntry& Candidate)
                    {
                        return IsLiveObject(
                                   Candidate
                                       .ItemDefinition) &&
                            Candidate.ItemDefinition
                                    ->Name.ToWString() ==
                                L"WID_AshtonPack_Milo";
                    },
                    FFortItemEntry::Size());
        if (!Entry)
            return false;

        auto& Focused =
            GAuthoredNativeLTMPhaseState
                .AshtonFocusedMiloPrimaryItems;
        const auto Existing =
            Focused.find(PlayerController);
        if (Existing != Focused.end() &&
            Existing->second == Entry->ItemGuid)
        {
            return true;
        }

        PlayerController
            ->ServerExecuteInventoryItem(
                Entry->ItemGuid);
        PlayerController->ClientEquipItem(
            Entry->ItemGuid, true);
        Focused[PlayerController] =
            Entry->ItemGuid;
        return true;
    }

    bool ReconcileAshtonMiloQuickbarSlots(
        AFortPlayerControllerAthena* PlayerController,
        const char* Reason)
    {
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(
                PlayerController->WorldInventory) ||
            !FFortItemEntry::HasOrderIndex())
        {
            return false;
        }

        auto Inventory =
            PlayerController->WorldInventory;
        auto& Rows =
            Inventory->Inventory.ReplicatedEntries;
        bool bChanged = false;
        UFortItemDefinition* PrimaryDefinition =
            nullptr;
        UFortItemDefinition* LauncherDefinition =
            nullptr;
        UFortItemDefinition* JetpackDefinition =
            nullptr;
        FGuid PrimaryGuid{};
        FGuid LauncherGuid{};
        FGuid JetpackGuid{};
        for (int32 Index = 0;
             Index < Rows.Num();
             ++Index)
        {
            auto& Entry = Rows.Get(
                Index, FFortItemEntry::Size());
            if (!IsLiveObject(
                    Entry.ItemDefinition))
            {
                continue;
            }

            const auto Name =
                Entry.ItemDefinition->Name.ToWString();
            int16 DesiredOrder = -1;
            if (Name == L"WID_AshtonPack_Milo")
            {
                DesiredOrder = 0;
                PrimaryDefinition =
                    const_cast<UFortItemDefinition*>(
                        Entry.ItemDefinition);
                PrimaryGuid = Entry.ItemGuid;
            }
            else if (
                Name ==
                L"WID_AshtonPack_Milo_Launcher")
            {
                DesiredOrder = 1;
                LauncherDefinition =
                    const_cast<UFortItemDefinition*>(
                        Entry.ItemDefinition);
                LauncherGuid = Entry.ItemGuid;
            }
            else if (
                Name ==
                L"AGID_JetPack_AshtonPack_Milo")
            {
                // FFortItemEntry::OrderIndex is zero-based. Leave slots 3
                // and 4 empty and pin the jetpack to the fifth/last combat
                // slot without touching the separate harvesting-tool slot.
                DesiredOrder = 4;
                JetpackDefinition =
                    const_cast<UFortItemDefinition*>(
                        Entry.ItemDefinition);
                JetpackGuid = Entry.ItemGuid;
            }
            if (DesiredOrder < 0 ||
                Entry.OrderIndex ==
                    DesiredOrder)
            {
                continue;
            }

            Entry.OrderIndex = DesiredOrder;
            const FGuid ItemGuid =
                Entry.ItemGuid;
            auto ItemInstance =
                Inventory->Inventory.ItemInstances.Search(
                    [&](UFortWorldItem* Item)
                    {
                        return IsLiveObject(Item) &&
                            Item->ItemEntry.ItemGuid.A ==
                                ItemGuid.A &&
                            Item->ItemEntry.ItemGuid.B ==
                                ItemGuid.B &&
                            Item->ItemEntry.ItemGuid.C ==
                                ItemGuid.C &&
                            Item->ItemEntry.ItemGuid.D ==
                                ItemGuid.D;
                    });
            if (ItemInstance &&
                *ItemInstance &&
                (*ItemInstance)->ItemEntry
                    .HasOrderIndex())
            {
                (*ItemInstance)->ItemEntry.OrderIndex =
                    DesiredOrder;
                (*ItemInstance)->ItemEntry.bIsDirty =
                    true;
            }
            Inventory->UpdateEntry(Entry);
            bChanged = true;
        }

        if (PrimaryDefinition &&
            LauncherDefinition &&
            JetpackDefinition &&
            PlayerController->GetFunction(
                "AddItemToQuickBars"))
        {
            auto& Tracked =
                GAuthoredNativeLTMPhaseState
                    .AshtonMiloQuickbarReconcileStates
                    [PlayerController];
            const uint64 PawnIdentity =
                GetCarminePassiveIdentity(
                    PlayerController->MyFortPawn);
            const bool bSameGeneration =
                Tracked.PawnIdentity ==
                    PawnIdentity &&
                Tracked.PrimaryGuid.A ==
                    PrimaryGuid.A &&
                Tracked.PrimaryGuid.B ==
                    PrimaryGuid.B &&
                Tracked.PrimaryGuid.C ==
                    PrimaryGuid.C &&
                Tracked.PrimaryGuid.D ==
                    PrimaryGuid.D &&
                Tracked.LauncherGuid.A ==
                    LauncherGuid.A &&
                Tracked.LauncherGuid.B ==
                    LauncherGuid.B &&
                Tracked.LauncherGuid.C ==
                    LauncherGuid.C &&
                Tracked.LauncherGuid.D ==
                    LauncherGuid.D &&
                Tracked.JetpackGuid.A ==
                    JetpackGuid.A &&
                Tracked.JetpackGuid.B ==
                    JetpackGuid.B &&
                Tracked.JetpackGuid.C ==
                    JetpackGuid.C &&
                Tracked.JetpackGuid.D ==
                    JetpackGuid.D;
            if (!bSameGeneration)
            {
                if (PlayerController->GetFunction(
                        "RemoveItemFromQuickBars"))
                {
                    PlayerController
                        ->RemoveItemFromQuickBars(
                            PrimaryDefinition);
                    PlayerController
                        ->RemoveItemFromQuickBars(
                            LauncherDefinition);
                    PlayerController
                        ->RemoveItemFromQuickBars(
                            JetpackDefinition);
                }

                // In 10.40 the client-only quickbar is driven by replicated
                // delayed actions. Raw primary slot zero is the pickaxe;
                // numbered weapon slots are 1..5.
                uint8 PrimaryQuickbar = 0;
                PlayerController->AddItemToQuickBars(
                    PrimaryDefinition,
                    PrimaryQuickbar,
                    1);
                PlayerController->AddItemToQuickBars(
                    LauncherDefinition,
                    PrimaryQuickbar,
                    2);
                PlayerController->AddItemToQuickBars(
                    JetpackDefinition,
                    PrimaryQuickbar,
                    5);
                if (PlayerController->GetFunction(
                        "ClientForceUpdateQuickbar"))
                {
                    PlayerController
                        ->ClientForceUpdateQuickbar(
                            PrimaryQuickbar);
                }

                Tracked.PawnIdentity =
                    PawnIdentity;
                Tracked.PrimaryGuid = PrimaryGuid;
                Tracked.LauncherGuid = LauncherGuid;
                Tracked.JetpackGuid = JetpackGuid;
                bChanged = true;
                SDK::DbgLog(
                    "[Ashton1040] queued native Chitauri "
                    "quickbar slots PC=%p laser=1 "
                    "launcher=2 jetpack=5\n",
                    static_cast<void*>(
                        PlayerController));
            }
        }

        if (bChanged)
        {
            Inventory->ForceNetUpdate();
            PlayerController->ForceNetUpdate();
            SDK::DbgLog(
                "[Ashton1040] reconciled Chitauri "
                "quickbar order PC=%p reason=%s\n",
                static_cast<void*>(
                    PlayerController),
                Reason ? Reason : "unknown");
        }
        return bChanged;
    }

    struct FAshtonMiloWeaponRuntimeState
    {
        bool bFound = false;
        bool bUsable = false;
    };

    void ObserveAshtonMiloWeaponActor(
        AActor* Actor,
        const wchar_t* DefinitionName,
        FAshtonMiloWeaponRuntimeState& State)
    {
        auto Weapon =
            IsLiveObject(Actor)
                ? Actor->Cast<AFortWeapon>()
                : nullptr;
        if (!IsLiveObject(Weapon) ||
            !Weapon->HasWeaponData() ||
            !IsLiveObject(Weapon->WeaponData) ||
            Weapon->WeaponData->Name.ToWString() !=
                DefinitionName)
        {
            return;
        }

        State.bFound = true;
        const bool bHasAmmo =
            !Weapon->HasAmmoCount() ||
            Weapon->AmmoCount > 0;
        const bool bCompletedLoad =
            !Weapon->HasbCompletedWeaponLoad() ||
            Weapon->bCompletedWeaponLoad;
        State.bUsable =
            State.bUsable ||
            (bHasAmmo && bCompletedLoad);
    }

    FAshtonMiloWeaponRuntimeState
        InspectAshtonMiloWeaponRuntime(
            AFortPlayerControllerAthena* PlayerController,
            const wchar_t* DefinitionName)
    {
        FAshtonMiloWeaponRuntimeState State{};
        auto Pawn =
            IsLiveObject(PlayerController)
                ? PlayerController->MyFortPawn
                : nullptr;
        if (!IsLiveObject(Pawn) || !DefinitionName)
            return State;

        if (Pawn->HasCurrentWeapon())
        {
            ObserveAshtonMiloWeaponActor(
                Pawn->CurrentWeapon,
                DefinitionName,
                State);
        }
        if (!Pawn->HasCurrentWeaponList())
            return State;

        const auto& Weapons =
            Pawn->CurrentWeaponList;
        if (!IsSaneArray(
                Weapons.Num(), Weapons.Max(), 64) ||
            !IsReadableArrayStorage(
                Weapons.Data,
                Weapons.Num(),
                sizeof(AActor*)))
        {
            return State;
        }
        for (int32 Index = 0;
             Index < Weapons.Num();
             ++Index)
        {
            ObserveAshtonMiloWeaponActor(
                Weapons.Get(Index),
                DefinitionName,
                State);
        }
        return State;
    }

    bool RecoverMissingAshtonMiloProducts(
        AFortPlayerControllerAthena* PlayerController,
        const FGuid& MainGuid,
        const char* Reason)
    {
        (void)MainGuid;
        auto& State = GAuthoredNativeLTMPhaseState;
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(PlayerController->WorldInventory))
        {
            return false;
        }

        auto Inventory =
            PlayerController->WorldInventory;
        bool bHasJetpack = false;
        bool bHasLoadedLauncherRow = false;
        bool bHasLoadedPrimaryRow = false;
        std::vector<FGuid> ChildRows;
        auto& Rows =
            Inventory->Inventory.ReplicatedEntries;
        for (int32 Index = 0;
             Index < Rows.Num();
             ++Index)
        {
            auto& Entry = Rows.Get(
                Index, FFortItemEntry::Size());
            if (!IsLiveObject(Entry.ItemDefinition))
                continue;
            const auto Name =
                Entry.ItemDefinition->Name.ToWString();
            if (Name ==
                L"AGID_JetPack_AshtonPack_Milo")
            {
                bHasJetpack = true;
                ChildRows.push_back(
                    Entry.ItemGuid);
            }
            else if (Name ==
                L"WID_AshtonPack_Milo_Launcher")
            {
                bHasLoadedLauncherRow =
                    bHasLoadedLauncherRow ||
                    Entry.LoadedAmmo > 0;
                ChildRows.push_back(
                    Entry.ItemGuid);
            }
            else if (Name ==
                L"WID_AshtonPack_Milo")
            {
                // The primary is an ability-equipped transient weapon in
                // the authored 10.40 path. A row exists only after the final
                // compatibility fallback.
                bHasLoadedPrimaryRow =
                    bHasLoadedPrimaryRow ||
                    Entry.LoadedAmmo > 0;
                ChildRows.push_back(
                    Entry.ItemGuid);
            }
        }

        const auto PrimaryRuntime =
            InspectAshtonMiloWeaponRuntime(
                PlayerController,
                L"WID_AshtonPack_Milo");
        const auto LauncherRuntime =
            InspectAshtonMiloWeaponRuntime(
                PlayerController,
                L"WID_AshtonPack_Milo_Launcher");
        const bool bHasUsablePrimary =
            bHasLoadedPrimaryRow ||
            PrimaryRuntime.bUsable;
        const bool bHasUsableLauncher =
            bHasLoadedLauncherRow &&
            (!LauncherRuntime.bFound ||
             LauncherRuntime.bUsable);
        if (bHasUsablePrimary &&
            bHasJetpack &&
            bHasUsableLauncher)
        {
            return false;
        }

        if (State.AshtonMiloFallbackAttempted
                .contains(PlayerController))
        {
            return false;
        }
        State.AshtonMiloFallbackAttempted.insert(
            PlayerController);

        const auto JetpackDefinition =
            FindAshtonVillainGear(
                L"AGID_JetPack_AshtonPack_Milo");
        const auto LauncherDefinition =
            FindAshtonVillainGear(
                L"WID_AshtonPack_Milo_Launcher");
        const auto PrimaryDefinition =
            FindAshtonVillainGear(
                L"WID_AshtonPack_Milo");
        int32 FallbackRows = 0;
        if (!bHasUsableLauncher &&
            IsLiveObject(LauncherDefinition))
        {
            // Remove every zero-ammo copy before creating a definition-derived
            // entry. MakeItemEntry reads the exact weapon stats and produces
            // the authored 1,000,000-round clip; the launcher has no reload
            // ability and cannot recover from a literal zero.
            for (const auto Guid : ChildRows)
            {
                auto Entry =
                    Rows.Search(
                        [&](FFortItemEntry& Candidate)
                        {
                            return Candidate.ItemGuid.A ==
                                    Guid.A &&
                                Candidate.ItemGuid.B ==
                                    Guid.B &&
                                Candidate.ItemGuid.C ==
                                    Guid.C &&
                                Candidate.ItemGuid.D ==
                                    Guid.D &&
                                IsLiveObject(
                                    Candidate
                                        .ItemDefinition) &&
                                Candidate.ItemDefinition
                                        ->Name.ToWString() ==
                                    L"WID_AshtonPack_Milo_Launcher";
                        },
                        FFortItemEntry::Size());
                if (Entry)
                    Inventory->Remove(Guid);
            }
            auto Entry = AFortInventory::MakeItemEntry(
                LauncherDefinition, 1, -1);
            if (Entry)
            {
                PlayerController->InternalPickup(Entry);
                free(Entry);
                ++FallbackRows;
            }
        }
        if (!bHasJetpack &&
            IsLiveObject(JetpackDefinition))
        {
            auto Entry = AFortInventory::MakeItemEntry(
                JetpackDefinition, 1, -1);
            if (Entry)
            {
                PlayerController->InternalPickup(Entry);
                free(Entry);
                ++FallbackRows;
            }
        }
        if (!bHasUsablePrimary &&
            IsLiveObject(PrimaryDefinition))
        {
            // EquipAbilityWeapon is the authored representation. If two
            // bounded stock-gadget activation attempts still produced no
            // usable transient rifle, expose the exact weapon as a normal
            // stat-derived row so the player is never left defenseless.
            for (const auto Guid : ChildRows)
            {
                auto Entry =
                    Rows.Search(
                        [&](FFortItemEntry& Candidate)
                        {
                            return Candidate.ItemGuid.A ==
                                    Guid.A &&
                                Candidate.ItemGuid.B ==
                                    Guid.B &&
                                Candidate.ItemGuid.C ==
                                    Guid.C &&
                                Candidate.ItemGuid.D ==
                                    Guid.D &&
                                IsLiveObject(
                                    Candidate
                                        .ItemDefinition) &&
                                Candidate.ItemDefinition
                                        ->Name.ToWString() ==
                                    L"WID_AshtonPack_Milo";
                        },
                        FFortItemEntry::Size());
                if (Entry)
                    Inventory->Remove(Guid);
            }
            auto Entry = AFortInventory::MakeItemEntry(
                PrimaryDefinition, 1, -1);
            if (Entry)
            {
                auto GrantedPrimary =
                    Inventory->GiveItem(
                        *Entry, 1, true, true);
                if (GrantedPrimary)
                {
                    const FGuid PrimaryGuid =
                        GrantedPrimary
                            ->ItemEntry.ItemGuid;
                    PlayerController
                        ->ServerExecuteInventoryItem(
                            PrimaryGuid);
                    PlayerController
                        ->ClientEquipItem(
                            PrimaryGuid, true);
                    ++FallbackRows;
                }
                free(Entry);
            }
        }
        ReconcileAshtonMiloQuickbarSlots(
            PlayerController, Reason);
        Inventory->ForceNetUpdate();
        PlayerController->ForceNetUpdate();
        SDK::DbgLog(
            "[Ashton1040] applied bounded Milo "
            "product fallback PC=%p rows=%d "
            "primaryBefore=%d jetpackBefore=%d "
            "launcherBefore=%d reason=%s\n",
            static_cast<void*>(PlayerController),
            FallbackRows,
            bHasUsablePrimary ? 1 : 0,
            bHasJetpack ? 1 : 0,
            bHasUsableLauncher ? 1 : 0,
            Reason ? Reason : "unknown");
        return FallbackRows > 0;
    }

    bool ReconcileAshtonVillainPlayer(
        AFortPlayerControllerAthena* PlayerController,
        const char* Reason)
    {
        auto& State = GAuthoredNativeLTMPhaseState;
        if (!IsLiveObject(PlayerController))
            return false;

        auto RawPublishedLeader =
            State.AshtonMutator &&
                    State.AshtonMutator
                        ->HasVillainLeaderPC()
                ? State.AshtonMutator
                      ->VillainLeaderPC
                : nullptr;
        const bool bAuthoritativeLeader =
            !State.bAshtonLeaderVacant &&
            RawPublishedLeader == PlayerController &&
            IsValidAshtonLeaderController(
                PlayerController);
        if (!bAuthoritativeLeader)
        {
            // Artifact cleanup is inventory-only and must not depend on a
            // living/ready pawn or even villain team membership. This sweeps
            // dead former carriers, heroes and pre-possession transfers too.
            RemoveAllAshtonLeaderArtifacts(
                State.AshtonMutator,
                PlayerController);
        }

        if (!PlayerController->HasPlayerState() ||
            !IsLiveObject(PlayerController->PlayerState) ||
            !IsLiveObject(PlayerController->WorldInventory) ||
            !PlayerController->HasMyFortPawn() ||
            !IsLiveObject(PlayerController->MyFortPawn))
        {
            return false;
        }

        auto ControlledPawn =
            PlayerController->MyFortPawn;
        if (ControlledPawn->GetHealth() <= 0.0f ||
            (ControlledPawn->HasbIsDying() &&
             ControlledPawn->bIsDying) ||
            (ControlledPawn->HasbPlayedDying() &&
             ControlledPawn->bPlayedDying) ||
            (ControlledPawn->HasbIsDBNO() &&
             ControlledPawn->bIsDBNO))
        {
            return false;
        }

        const auto ReadyPawn =
            State.AshtonReadyPawns.find(
                PlayerController);
        if (ReadyPawn ==
                State.AshtonReadyPawns.end() ||
            ReadyPawn->second != ControlledPawn)
        {
            // Only initialize a pawn after its authoritative possession-ready
            // callback. Otherwise a watchdog can notify Milo immediately
            // before pawn-ready clears the per-pawn GUID state and notify the
            // same gadget a second time.
            return false;
        }

        auto PlayerState =
            static_cast<AFortPlayerStateAthena*>(
                PlayerController->PlayerState);
        if (!PlayerState->HasTeamIndex() ||
            PlayerState->TeamIndex !=
                State.AshtonVillainTeam)
        {
            return false;
        }

        auto Inventory =
            PlayerController->WorldInventory;
        auto PublishedLeader =
            State.AshtonMutator &&
                    State.AshtonMutator
                        ->HasVillainLeaderPC()
                ? State.AshtonMutator
                      ->VillainLeaderPC
                : nullptr;
        bool bHasValidPublishedLeader =
            !State.bAshtonLeaderVacant &&
            IsValidAshtonLeaderController(
                PublishedLeader);
        if (!bHasValidPublishedLeader &&
            State.AshtonMutator &&
            State.AshtonMutator->HasVillainLeaderPC() &&
            (PublishedLeader ||
             !State.bAshtonLeaderVacant))
        {
            // A dead/stale native leader never authorizes its inventory row to
            // republish ownership. The slot remains vacant until a proven
            // stone capture chooses the next leader.
            State.bAshtonLeaderVacant = true;
            State.AshtonMutator->VillainLeaderPC = nullptr;
            State.AshtonMutator->ForceNetUpdate();
            PublishedLeader = nullptr;
        }
        const bool bIsLeader =
            bHasValidPublishedLeader &&
            PublishedLeader == PlayerController;

        bool bChanged = false;
        if (bIsLeader)
        {
            State.AshtonMiloQuickbarReconcileStates.erase(
                PlayerController);
        }
        else
        {
            bChanged =
                ReconcileAshtonMiloQuickbarSlots(
                    PlayerController, Reason) ||
                bChanged;
        }

        std::vector<FGuid> InvalidInventoryRows;
        auto& InventoryRows =
            Inventory->Inventory.ReplicatedEntries;
        std::unordered_map<
            const UFortItemDefinition*,
            FGuid> PreferredVillainGuids;
        std::unordered_set<
            const UFortItemDefinition*>
                SeenVillainDefinitions;
        if (!bIsLeader)
        {
            const auto Initialized =
                State.AshtonInitializedVillainItems
                    .find(PlayerController);
            if (Initialized !=
                State.AshtonInitializedVillainItems.end())
            {
                for (const auto& [Definition, Guid] :
                     Initialized->second)
                {
                    const bool bGuidStillPresent =
                        InventoryRows.Search(
                            [&](FFortItemEntry& Entry)
                            {
                                return Entry.ItemDefinition ==
                                        Definition &&
                                    Entry.ItemGuid.A == Guid.A &&
                                    Entry.ItemGuid.B == Guid.B &&
                                    Entry.ItemGuid.C == Guid.C &&
                                    Entry.ItemGuid.D == Guid.D;
                            },
                            FFortItemEntry::Size()) !=
                            nullptr;
                    if (bGuidStillPresent)
                    {
                        PreferredVillainGuids[
                            Definition] = Guid;
                    }
                }
            }
        }

        // If an older compatibility pass raced Milo's passive, prefer the
        // stat-initialized launcher over a zero-ammo synthetic row. The 10.40
        // launcher has no reload ability, so keeping the first GUID blindly can
        // permanently replace the authored 1,000,000-round energy clip with an
        // unusable weapon.
        const UFortItemDefinition* PreferredLauncherDefinition =
            nullptr;
        FGuid PreferredLauncherGuid{};
        int32 PreferredLauncherAmmo =
            (std::numeric_limits<int32>::min)();
        for (int32 Index = 0;
             Index < InventoryRows.Num();
             ++Index)
        {
            auto& Entry = InventoryRows.Get(
                Index, FFortItemEntry::Size());
            if (!IsLiveObject(Entry.ItemDefinition) ||
                Entry.ItemDefinition->Name.ToWString() !=
                    L"WID_AshtonPack_Milo_Launcher" ||
                Entry.LoadedAmmo <= PreferredLauncherAmmo)
            {
                continue;
            }
            PreferredLauncherDefinition =
                Entry.ItemDefinition;
            PreferredLauncherGuid = Entry.ItemGuid;
            PreferredLauncherAmmo = Entry.LoadedAmmo;
        }
        if (PreferredLauncherDefinition)
        {
            PreferredVillainGuids[
                PreferredLauncherDefinition] =
                PreferredLauncherGuid;
        }

        InvalidInventoryRows.reserve(
            InventoryRows.Num());
        for (int32 Index = 0;
             Index < InventoryRows.Num();
             ++Index)
        {
            auto& Entry = InventoryRows.Get(
                Index, FFortItemEntry::Size());
            bool bAllowed =
                bIsLeader
                    ? IsAshtonLeaderInventoryDefinition(
                          State.AshtonMutator,
                          Entry.ItemDefinition)
                    : !IsAnyAshtonLeaderArtifact(
                          State.AshtonMutator,
                          Entry.ItemDefinition) &&
                          (State.AshtonVillainGear
                               .contains(
                                   Entry.ItemDefinition) ||
                           IsAshtonVillainUtilityDefinition(
                               Entry.ItemDefinition));
            if (bAllowed && !bIsLeader)
            {
                const auto Preferred =
                    PreferredVillainGuids.find(
                        Entry.ItemDefinition);
                if (Preferred !=
                    PreferredVillainGuids.end())
                {
                    const auto& Guid = Preferred->second;
                    bAllowed =
                        Entry.ItemGuid.A == Guid.A &&
                        Entry.ItemGuid.B == Guid.B &&
                        Entry.ItemGuid.C == Guid.C &&
                        Entry.ItemGuid.D == Guid.D;
                }
                else
                {
                    bAllowed =
                        SeenVillainDefinitions.insert(
                            Entry.ItemDefinition).second;
                }
            }
            if (!bAllowed)
            {
                InvalidInventoryRows.push_back(
                    Entry.ItemGuid);
            }
        }
        for (const auto Guid : InvalidInventoryRows)
        {
            Inventory->Remove(Guid);
            bChanged = true;
        }

        if (bIsLeader)
        {
            State.AshtonInitializedVillainItems.erase(
                PlayerController);
            State.AshtonMiloChildrenNotBefore.erase(
                PlayerController);
            State.AshtonMiloRecoveryNotBefore.erase(
                PlayerController);
            State.AshtonMiloResetAttempted.erase(
                PlayerController);
            State.AshtonMiloFallbackAttempted.erase(
                PlayerController);
            State.AshtonFocusedMiloPrimaryItems.erase(
                PlayerController);
            // A native stone callback can publish VillainLeaderPC before its
            // gadget grant succeeds. Initialization itself is keyed by GUID and
            // remains strictly one-shot so replicated gameplay-cue tags cannot
            // accumulate on every watchdog pass.
            EnsureAshtonLeaderItem(
                State.AshtonMutator,
                PlayerController,
                Reason ? Reason :
                    "leader-watchdog");
            return bChanged;
        }

        if (State.AshtonEliminatedLeaders.contains(
                PlayerController))
        {
            // Do not populate the dead pawn's retained inventory. Player-ready
            // removes this gate for the replacement pawn and grants the exact
            // Chitauri loadout then.
            return bChanged;
        }

        // The authored inventory override can populate Milo before the base
        // inventory bootstrap runs. Restore only the player's harvesting and
        // build/edit tools here; materials remain controlled by the authored
        // mat-drip ability and the configured gameplay settings.
        bChanged =
            EnsureAshtonVillainUtilityItems(
                PlayerController, Reason) ||
            bChanged;

        for (const auto& LoadoutEntry :
             State.AshtonVillainLoadout)
        {
            auto Definition = LoadoutEntry.first;
            const int32 Count = LoadoutEntry.second;
            if (!IsLiveObject(Definition) || Count <= 0)
                continue;
            const bool bMainMiloGadget =
                Definition->Name.ToWString() ==
                    L"AGID_AshtonPack_Milo";
            const bool bMiloPrimary =
                Definition->Name.ToWString() ==
                    L"WID_AshtonPack_Milo";
            const double Now =
                UGameplayStatics::GetTimeSeconds(
                    UWorld::GetWorld());

            auto Existing =
                Inventory->Inventory.ReplicatedEntries.Search(
                    [Definition](FFortItemEntry& Entry)
                    {
                        return Entry.ItemDefinition ==
                            Definition;
                    },
                    FFortItemEntry::Size());
            const FGuid ExistingGuid =
                Existing
                    ? Existing->ItemGuid
                    : FGuid{};
            if (bMainMiloGadget)
            {
                auto Grace =
                    State.AshtonMiloChildrenNotBefore
                        .find(PlayerController);
                const bool bSameGraceGuid =
                    Existing &&
                    Grace !=
                        State.AshtonMiloChildrenNotBefore
                            .end() &&
                    Grace->second.first.A ==
                        ExistingGuid.A &&
                    Grace->second.first.B ==
                        ExistingGuid.B &&
                    Grace->second.first.C ==
                        ExistingGuid.C &&
                    Grace->second.first.D ==
                        ExistingGuid.D;
                if (bSameGraceGuid &&
                    Now < Grace->second.second)
                {
                    // The authored passive contains a 0.1-second latent step,
                    // but on a newly caught-up client its ability activation can
                    // arrive much later. Observe for the full bounded window
                    // before considering recovery.
                    return bChanged;
                }
                if (Grace !=
                        State.AshtonMiloChildrenNotBefore
                            .end() &&
                    (!bSameGraceGuid ||
                     Now >= Grace->second.second))
                {
                    State.AshtonMiloChildrenNotBefore
                        .erase(Grace);
                }
            }
            if (Existing)
            {
                auto PlayerItems =
                    State.AshtonInitializedVillainItems
                        .find(PlayerController);
                bool bAlreadyInitialized = false;
                if (PlayerItems !=
                    State.AshtonInitializedVillainItems
                        .end())
                {
                    const auto Initialized =
                        PlayerItems->second.find(
                            Definition);
                    bAlreadyInitialized =
                        Initialized !=
                            PlayerItems->second.end() &&
                        Initialized->second.A ==
                            ExistingGuid.A &&
                        Initialized->second.B ==
                            ExistingGuid.B &&
                        Initialized->second.C ==
                            ExistingGuid.C &&
                        Initialized->second.D ==
                            ExistingGuid.D;
                }
                if (bMainMiloGadget &&
                    bAlreadyInitialized &&
                    RecoverMissingAshtonMiloProducts(
                        PlayerController,
                        ExistingGuid,
                        Reason))
                {
                    return true;
                }
                if (!bAlreadyInitialized)
                {
                    const bool bRequiresActivation =
                        bMainMiloGadget;
                    bool bHasCompleteNativeProducts =
                        false;
                    if (bMainMiloGadget)
                    {
                        bool bHasJetpack = false;
                        bool bHasLoadedLauncher = false;
                        bool bHasLoadedPrimary = false;
                        auto& ProductRows =
                            Inventory->Inventory
                                .ReplicatedEntries;
                        for (int32 ProductIndex = 0;
                             ProductIndex <
                                 ProductRows.Num();
                             ++ProductIndex)
                        {
                            auto& ProductEntry =
                                ProductRows.Get(
                                    ProductIndex,
                                    FFortItemEntry::Size());
                            if (!IsLiveObject(
                                    ProductEntry
                                        .ItemDefinition))
                            {
                                continue;
                            }
                            const auto ProductName =
                                ProductEntry
                                    .ItemDefinition
                                    ->Name.ToWString();
                            bHasJetpack =
                                bHasJetpack ||
                                ProductName ==
                                    L"AGID_JetPack_AshtonPack_Milo";
                            bHasLoadedLauncher =
                                bHasLoadedLauncher ||
                                (ProductName ==
                                     L"WID_AshtonPack_Milo_Launcher" &&
                                 ProductEntry.LoadedAmmo >
                                     0);
                            bHasLoadedPrimary =
                                bHasLoadedPrimary ||
                                (ProductName ==
                                     L"WID_AshtonPack_Milo" &&
                                 ProductEntry.LoadedAmmo >
                                     0);
                        }
                        const auto PrimaryRuntime =
                            InspectAshtonMiloWeaponRuntime(
                                PlayerController,
                                L"WID_AshtonPack_Milo");
                        const auto LauncherRuntime =
                            InspectAshtonMiloWeaponRuntime(
                                PlayerController,
                                L"WID_AshtonPack_Milo_Launcher");
                        bHasCompleteNativeProducts =
                            (bHasLoadedPrimary ||
                             PrimaryRuntime.bUsable) &&
                            bHasJetpack &&
                            bHasLoadedLauncher &&
                            (!LauncherRuntime.bFound ||
                             LauncherRuntime.bUsable);
                    }
                    if (bMainMiloGadget &&
                        !bHasCompleteNativeProducts)
                    {
                        auto Recovery =
                            State.AshtonMiloRecoveryNotBefore
                                .find(PlayerController);
                        const bool bSameRecoveryGuid =
                            Recovery !=
                                State.AshtonMiloRecoveryNotBefore
                                    .end() &&
                            Recovery->second.first.A ==
                                ExistingGuid.A &&
                            Recovery->second.first.B ==
                                ExistingGuid.B &&
                            Recovery->second.first.C ==
                                ExistingGuid.C &&
                            Recovery->second.first.D ==
                                ExistingGuid.D;
                        if (!bSameRecoveryGuid)
                        {
                            // An authored InventoryOverride can have already
                            // sent the item-added callback while Milo's 0.1 s
                            // passive child grant is still pending. Observe
                            // first so the recovery callback cannot duplicate
                            // its abilities or persistent effects.
                            State.AshtonMiloRecoveryNotBefore[
                                PlayerController] = {
                                     ExistingGuid,
                                     Now + 0.75};
                            return bChanged;
                        }
                        if (Now <
                            Recovery->second.second)
                        {
                            return bChanged;
                        }
                        State.AshtonMiloRecoveryNotBefore
                            .erase(Recovery);
                    }
                    else if (bMainMiloGadget)
                    {
                        State.AshtonMiloRecoveryNotBefore
                            .erase(PlayerController);
                    }
                    auto ItemInstance =
                        bRequiresActivation &&
                                !bHasCompleteNativeProducts
                            ? Inventory->Inventory.ItemInstances
                                  .Search(
                                      [&](UFortWorldItem* Item)
                                      {
                                          return IsLiveObject(Item) &&
                                              Item->ItemEntry
                                                      .ItemGuid.A ==
                                                  ExistingGuid.A &&
                                              Item->ItemEntry
                                                      .ItemGuid.B ==
                                                  ExistingGuid.B &&
                                              Item->ItemEntry
                                                      .ItemGuid.C ==
                                                  ExistingGuid.C &&
                                              Item->ItemEntry
                                                      .ItemGuid.D ==
                                                  ExistingGuid.D;
                                      })
                            : nullptr;
                    const bool bActivationSucceeded =
                        !bRequiresActivation ||
                        bHasCompleteNativeProducts ||
                        (ItemInstance &&
                         *ItemInstance &&
                         Inventory
                             ->InitializeGadgetItemWithFallback(
                                 *ItemInstance, true));
                    if (bRequiresActivation &&
                        !bActivationSucceeded)
                    {
                        // No item-added callback was dispatched. Leave this
                        // GUID retryable; once the instance exists, exactly one
                        // successful stock notification will make it terminal.
                        SDK::DbgLog(
                            "[Ashton1040] villain gadget "
                            "activation deferred definition=%s "
                            "PC=%p reason=%s\n",
                            Definition->Name
                                .ToString().c_str(),
                            static_cast<void*>(
                                PlayerController),
                            Reason ? Reason : "unknown");
                        return bChanged;
                    }
                    State.AshtonInitializedVillainItems
                        [PlayerController][Definition] =
                        ExistingGuid;
                    if (bRequiresActivation)
                    {
                        SDK::DbgLog(
                            "[Ashton1040] observed existing "
                            "villain gadget definition=%s "
                            "PC=%p activationSucceeded=%d "
                            "reason=%s\n",
                            Definition->Name
                                .ToString().c_str(),
                            static_cast<void*>(
                                PlayerController),
                            bActivationSucceeded ? 1 : 0,
                            Reason ? Reason : "unknown");
                    }
                    if (bMainMiloGadget)
                    {
                        if (!bHasCompleteNativeProducts)
                        {
                            State.AshtonMiloChildrenNotBefore[
                                 PlayerController] = {
                                     ExistingGuid,
                                     Now + 0.75};
                        }
                        // Let the passive equip its transient primary weapon,
                        // launcher and jetpack in authored order. Child rows are
                        // never synthesized by the normal loadout loop.
                        return bChanged;
                    }
                }
                if (bMiloPrimary)
                {
                    FocusAshtonMiloPrimaryOnce(
                        PlayerController);
                }
                continue;
            }

            // Use the ordinary inventory path here. It performs the local
            // inventory update and dispatches UFortWorldItem's native
            // OnItemInstanceAdded callback, which is the stock path that
            // applies a gadget's ability set. The former deferred grant
            // suppressed that callback and then deleted the item whenever a
            // private ApplyGadgetData address was unavailable.
            bool bGadgetInitializationDispatched =
                false;
            UFortWorldItem* GrantedItem = nullptr;
            if (bMiloPrimary)
            {
                // A literal LoadedAmmo=0 produces a visible rifle that cannot
                // fire. Build the row from WID stats so its authored clip is
                // present immediately on spawn island and every respawn.
                auto PrimaryEntry =
                    AFortInventory::MakeItemEntry(
                        Definition, Count, -1);
                if (PrimaryEntry)
                {
                    GrantedItem = Inventory->GiveItem(
                        *PrimaryEntry,
                        Count,
                        true,
                        true);
                    free(PrimaryEntry);
                }
            }
            else
            {
                GrantedItem = Inventory->GiveItem(
                    Definition,
                    Count,
                    0,
                    0,
                    true,
                    true,
                    0,
                    {},
                    true,
                    {},
                    nullptr,
                    bMainMiloGadget
                        ? &bGadgetInitializationDispatched
                        : nullptr);
            }
            if (!GrantedItem)
                continue;

            // A successfully created inventory gadget remains authoritative
            // even if the optional private ApplyGadgetData fallback cannot be
            // resolved for a shifted executable. Never erase visible,
            // callback-initialized Chitauri gear on that basis.
            if (bMainMiloGadget &&
                !bGadgetInitializationDispatched)
            {
                // The row is valid, but without a confirmed item-added
                // callback its main ability set is not terminal. Seed the
                // observation window so a later ready owner/interface gets
                // one recovery notification, never zero or two.
                State.AshtonMiloRecoveryNotBefore[
                    PlayerController] = {
                         GrantedItem->ItemEntry.ItemGuid,
                         Now + 0.75};
                bChanged = true;
                SDK::DbgLog(
                    "[Ashton1040] granted villain main "
                    "but deferred its callback definition=%s "
                    "PC=%p reason=%s\n",
                    Definition->Name
                        .ToString().c_str(),
                    static_cast<void*>(
                        PlayerController),
                    Reason ? Reason : "unknown");
                return bChanged;
            }
            State.AshtonInitializedVillainItems
                [PlayerController][Definition] =
                GrantedItem->ItemEntry.ItemGuid;
            if (bMainMiloGadget)
            {
                State.AshtonMiloRecoveryNotBefore.erase(
                    PlayerController);
            }
            bChanged = true;
            SDK::DbgLog(
                "[Ashton1040] granted villain loadout "
                "definition=%s count=%d PC=%p reason=%s\n",
                Definition->Name.ToString().c_str(),
                Count,
                static_cast<void*>(PlayerController),
                Reason ? Reason : "unknown");
            if (bMiloPrimary)
            {
                FocusAshtonMiloPrimaryOnce(
                    PlayerController);
            }
            if (bMainMiloGadget)
            {
                State.AshtonMiloChildrenNotBefore[
                     PlayerController] = {
                         GrantedItem->ItemEntry.ItemGuid,
                         Now + 0.75};
                // Do not race the main gadget's delayed passive grants.
                return bChanged;
            }
        }
        return bChanged;
    }

    void ReconcileAshtonVillainPlayers(
        AFortGameStateAthena* GameState,
        const char* Reason)
    {
        if (!EnsureAshtonVillainConfiguration(GameState) ||
            !GameState->HasPlayerArray())
        {
            return;
        }

        auto& Players = GameState->PlayerArray;
        if (!IsSaneArray(
                Players.Num(), Players.Max(), 256) ||
            !IsReadableArrayStorage(
                Players.Data,
                Players.Num(),
                sizeof(AFortPlayerStateAthena*)))
        {
            return;
        }
        for (auto PlayerState : Players)
        {
            auto PlayerController =
                IsLiveObject(PlayerState) &&
                        PlayerState->HasOwner() &&
                        IsLiveObject(PlayerState->Owner)
                    ? PlayerState->Owner
                          ->Cast<
                              AFortPlayerControllerAthena>()
                    : nullptr;
            ReconcileAshtonVillainPlayer(
                PlayerController, Reason);
        }
        ReconcileAshtonStoneHealthEffects(
            GameState,
            GAuthoredNativeLTMPhaseState
                .AshtonMutator,
            Reason);
    }

    bool EnsureAshtonGameplayDelegateBindings(
        AFortGameStateAthena* GameState,
        AFortGameplayMutator* Mutator)
    {
        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        if (!IsLiveObject(GameState) ||
            !IsLiveObject(Mutator) ||
            !GameState->HasOnPickupSpawnedAndReady() ||
            !GameState->HasOnPickupDestroy() ||
            !GameState->HasMutatorGameplayEvent())
        {
            return false;
        }
        if (PhaseState.AshtonDelegateBindingsComplete
                .contains(Mutator))
        {
            return true;
        }

        const bool bPickupReady =
            EnsureAuthoredDelegateBinding(
                GameState->OnPickupSpawnedAndReady,
                Mutator,
                L"OnPickupSpawnedAndReady");
        const bool bPickupDestroy =
            EnsureAuthoredDelegateBinding(
                GameState->OnPickupDestroy,
                Mutator,
                L"OnPickupDestroying");
        const bool bGameplayEvent =
            EnsureAuthoredDelegateBinding(
                GameState->MutatorGameplayEvent,
                Mutator,
                L"OnMutatorGameplayEvent");
        if (!bPickupReady ||
            !bPickupDestroy ||
            !bGameplayEvent)
        {
            return false;
        }

        PhaseState.AshtonDelegateBindingsComplete
            .insert(Mutator);
        SDK::DbgLog(
            "[Ashton1040] verified pickup-ready, "
            "pickup-destroy and gameplay-event bindings "
            "on %s\n",
            Mutator->Name.ToString().c_str());
        return true;
    }

    bool HasInitializedAshtonStoneRuntime(
        AFortGameplayMutator* Mutator)
    {
        const UClass* AshtonClass =
            AFortAthenaMutator_Ashton::StaticClass();
        auto Ashton =
            IsLiveObject(Mutator) &&
                    AshtonClass &&
                    Mutator->IsA(AshtonClass)
                ? static_cast<AFortAthenaMutator_Ashton*>(
                      Mutator)
                : nullptr;
        if (!Ashton ||
            !Ashton->HasStoneList() ||
            !FAshtonStoneState::StaticStruct() ||
            !FAshtonStoneState::HasStoneType() ||
            !FAshtonStoneState::HasStoneState())
        {
            return false;
        }

        constexpr int32 AshtonStoneCount = 6;
        constexpr int32 AshtonStoneStateSize1040 = 0x18;
        const int32 EntrySize =
            FAshtonStoneState::Size();
        auto& Stones = Ashton->StoneList;
        if (EntrySize != AshtonStoneStateSize1040 ||
            !IsSaneArray(
                Stones.Num(), Stones.Max(), 16) ||
            Stones.Num() != AshtonStoneCount ||
            !IsReadableArrayStorage(
                Stones.Data,
                Stones.Num(),
                static_cast<size_t>(EntrySize)))
        {
            return false;
        }

        bool SeenStoneTypes[AshtonStoneCount]{};
        for (int32 Index = 0;
             Index < Stones.Num();
             ++Index)
        {
            const auto& Stone =
                Stones.Get(Index, EntrySize);
            if (Stone.StoneType >= AshtonStoneCount ||
                Stone.StoneState > 2 ||
                SeenStoneTypes[Stone.StoneType])
            {
                return false;
            }
            SeenStoneTypes[Stone.StoneType] = true;
        }
        return std::all_of(
            std::begin(SeenStoneTypes),
            std::end(SeenStoneTypes),
            [](bool bSeen)
            {
                return bSeen;
            });
    }

    constexpr uint8 AshtonStoneStateSpawned = 1;
    constexpr uint8 AshtonStoneStateCaptured = 2;

    UClass* ResolveAshtonRockPickupClass()
    {
        static UClass* PickupClass = nullptr;
        if (!IsLiveObject(PickupClass))
        {
            PickupClass =
                const_cast<UClass*>(
                    FindObject<UClass>(
                        L"/Game/Athena/Playlists/Ashton/"
                        L"Rocks/AshtonRockPickupBase."
                        L"AshtonRockPickupBase_C"));
        }

        static const UClass* GameModePickupClass = nullptr;
        if (!GameModePickupClass)
            GameModePickupClass =
                FindClass("FortGameModePickup");
        auto DefaultObject =
            IsLiveObject(PickupClass)
                ? PickupClass->GetDefaultObj()
                : nullptr;
        return IsLiveObject(DefaultObject) &&
                GameModePickupClass &&
                DefaultObject->IsA(GameModePickupClass)
            ? PickupClass
            : nullptr;
    }

    FAshtonStoneState* FindAshtonStoneState(
        AFortAthenaMutator_Ashton* Ashton,
        int32 StoneType)
    {
        if (!IsLiveObject(Ashton) ||
            !HasInitializedAshtonStoneRuntime(Ashton) ||
            StoneType < 0 ||
            StoneType >= 6)
        {
            return nullptr;
        }

        const int32 EntrySize =
            FAshtonStoneState::Size();
        auto& Stones = Ashton->StoneList;
        for (int32 Index = 0;
             Index < Stones.Num();
             ++Index)
        {
            auto& Stone =
                Stones.Get(Index, EntrySize);
            if (Stone.StoneType == StoneType)
                return &Stone;
        }
        return nullptr;
    }

    int32 CountCapturedAshtonStones(
        AFortAthenaMutator_Ashton* Ashton)
    {
        if (!IsLiveObject(Ashton) ||
            !HasInitializedAshtonStoneRuntime(Ashton))
        {
            return 0;
        }

        int32 Count = 0;
        const int32 EntrySize =
            FAshtonStoneState::Size();
        auto& Stones = Ashton->StoneList;
        for (int32 Index = 0;
             Index < Stones.Num();
             ++Index)
        {
            if (Stones.Get(Index, EntrySize)
                    .StoneState ==
                AshtonStoneStateCaptured)
            {
                ++Count;
            }
        }
        return Count;
    }

    bool ResolveCurrentAshtonStoneRuntime(
        const UFortItemDefinition* ItemDefinition,
        UWorld*& OutWorld,
        AFortGameStateAthena*& OutGameState,
        AFortAthenaMutator_Ashton*& OutAshton,
        int32& OutStoneType)
    {
        OutWorld = UWorld::GetWorld();
        OutGameState =
            OutWorld && OutWorld->GameState
                ? OutWorld->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        OutAshton = nullptr;
        OutStoneType =
            GetAshtonStoneType(ItemDefinition);
        if (OutStoneType < 0 ||
            !FFortAthenaNativeLTMCompatibility::
                IsSupportedBuild() ||
            !OutWorld ||
            !IsLiveObject(OutGameState) ||
            GNativeLTMCompatibilityState.World !=
                OutWorld ||
            !IsNativeAshtonDescriptor(
                GNativeLTMCompatibilityState.Descriptor))
        {
            return false;
        }

        // Team-loadout streaming is independent from the stone state array.
        // Attempt it so the authored team is known, but never make objective
        // capture depend on a soft gadget package resolving in this frame.
        EnsureAshtonVillainConfiguration(
            OutGameState);

        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        if (IsLiveObject(PhaseState.AshtonMutator))
        {
            OutAshton = PhaseState.AshtonMutator;
        }
        else
        {
            auto Candidate =
                FindAuthoredNativeLTMMutator(
                    OutGameState,
                    GNativeLTMCompatibilityState
                        .Descriptor);
            const UClass* AshtonClass =
                AFortAthenaMutator_Ashton::StaticClass();
            if (IsLiveObject(Candidate) &&
                AshtonClass &&
                Candidate->IsA(AshtonClass))
            {
                OutAshton =
                    static_cast<
                        AFortAthenaMutator_Ashton*>(
                        Candidate);
                PhaseState.AshtonMutator =
                    OutAshton;
            }
        }

        return IsLiveObject(OutAshton) &&
            OutAshton->HasAuthority() &&
            HasInitializedAshtonStoneRuntime(
                OutAshton);
    }

    bool ResolveAshtonVillainCollector(
        AFortPlayerPawnAthena* Pawn,
        AFortPlayerControllerAthena*&
            OutPlayerController)
    {
        OutPlayerController = nullptr;
        if (!IsLiveObject(Pawn) ||
            !Pawn->HasController() ||
            !IsLiveObject(Pawn->Controller))
        {
            return false;
        }

        auto PlayerController =
            Pawn->Controller
                ->Cast<AFortPlayerControllerAthena>();
        if (!IsLiveObject(PlayerController) ||
            !PlayerController->HasPlayerState() ||
            !IsLiveObject(
                PlayerController->PlayerState) ||
            !IsLiveObject(
                PlayerController->WorldInventory) ||
            GAuthoredNativeLTMPhaseState
                .AshtonEliminatedLeaders.contains(
                    PlayerController) ||
            !PlayerController->HasMyFortPawn() ||
            !IsLiveObject(PlayerController->MyFortPawn))
        {
            return false;
        }

        auto ControlledPawn =
            PlayerController->MyFortPawn;
        if (ControlledPawn->GetHealth() <= 0.0f ||
            (ControlledPawn->HasbIsDying() &&
             ControlledPawn->bIsDying) ||
            (ControlledPawn->HasbPlayedDying() &&
             ControlledPawn->bPlayedDying) ||
            (ControlledPawn->HasbIsDBNO() &&
             ControlledPawn->bIsDBNO))
        {
            return false;
        }

        auto PlayerState =
            static_cast<AFortPlayerStateAthena*>(
                PlayerController->PlayerState);
        const uint8 VillainTeam =
            GAuthoredNativeLTMPhaseState
                    .AshtonVillainTeam != 255
                ? GAuthoredNativeLTMPhaseState
                      .AshtonVillainTeam
                : 3;
        if (!PlayerState->HasTeamIndex() ||
            PlayerState->TeamIndex != VillainTeam)
        {
            return false;
        }

        OutPlayerController = PlayerController;
        return true;
    }

    bool IsValidAshtonLeaderController(
        AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsLiveObject(PlayerController) ||
            !PlayerController->HasPlayerState() ||
            !IsLiveObject(
                PlayerController->PlayerState) ||
            !IsLiveObject(
                PlayerController->WorldInventory) ||
            GAuthoredNativeLTMPhaseState
                .AshtonEliminatedLeaders.contains(
                    PlayerController) ||
            !PlayerController->HasMyFortPawn() ||
            !IsLiveObject(PlayerController->MyFortPawn))
        {
            return false;
        }

        auto Pawn = PlayerController->MyFortPawn;
        if (Pawn->GetHealth() <= 0.0f ||
            (Pawn->HasbIsDying() && Pawn->bIsDying) ||
            (Pawn->HasbPlayedDying() &&
             Pawn->bPlayedDying) ||
            (Pawn->HasbIsDBNO() && Pawn->bIsDBNO))
        {
            return false;
        }

        auto PlayerState =
            static_cast<AFortPlayerStateAthena*>(
                PlayerController->PlayerState);
        const uint8 VillainTeam =
            GAuthoredNativeLTMPhaseState
                    .AshtonVillainTeam != 255
                ? GAuthoredNativeLTMPhaseState
                      .AshtonVillainTeam
                : 3;
        return PlayerState->HasTeamIndex() &&
            PlayerState->TeamIndex == VillainTeam;
    }

    int32 RemoveAshtonInventoryRows(
        AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ExactDefinition,
        bool bRemoveEveryStone)
    {
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(
                PlayerController->WorldInventory))
        {
            return 0;
        }

        auto Inventory =
            PlayerController->WorldInventory;
        auto& Rows =
            Inventory->Inventory.ReplicatedEntries;
        if (!IsSaneArray(
                Rows.Num(), Rows.Max(), 256) ||
            !IsReadableArrayStorage(
                Rows.Data,
                Rows.Num(),
                static_cast<size_t>(
                    FFortItemEntry::Size())))
        {
            return 0;
        }

        std::vector<FGuid> RowsToRemove;
        RowsToRemove.reserve(Rows.Num());
        for (int32 Index = 0;
             Index < Rows.Num();
             ++Index)
        {
            auto& Entry =
                Rows.Get(
                    Index,
                    FFortItemEntry::Size());
            if ((ExactDefinition &&
                 Entry.ItemDefinition ==
                     ExactDefinition) ||
                (bRemoveEveryStone &&
                 IsAshtonStoneItemDefinition(
                     Entry.ItemDefinition)))
            {
                RowsToRemove.push_back(
                    Entry.ItemGuid);
            }
        }

        for (const auto Guid : RowsToRemove)
            Inventory->Remove(Guid);
        return static_cast<int32>(
            RowsToRemove.size());
    }

    void CancelAshtonLeaderPromotion(
        AFortPlayerControllerAthena* PlayerController)
    {
        auto& State = GAuthoredNativeLTMPhaseState;
        if (PlayerController &&
            State.AshtonPromotionController !=
                PlayerController)
        {
            return;
        }
        State.AshtonPromotionController = nullptr;
        State.AshtonPromotionPawn = nullptr;
        State.AshtonPromotionItemGuid = {};
        State.AshtonPromotionStartedAt = 0.0;
        State.AshtonPromotionStartZ = 0.0;
        State.AshtonPromotionReadyObservedAt = 0.0;
        State.AshtonPromotionReadyFallbackAt = 0.0;
        State.AshtonPromotionTeleportFallbackAt = 0.0;
        State.AshtonPromotionSkydiveFallbackAt = 0.0;
        State.bAshtonPromotionUsesCarmine = false;
        State.bAshtonPromotionReadyFallbackInvoked = false;
        State.bAshtonPromotionTeleportFallbackInvoked = false;
        State.bAshtonPromotionSkydiveFallbackInvoked = false;
        State.bAshtonPromotionTeleported = false;
        State.bAshtonPromotionSkydiving = false;
    }

    UAbilitySystemComponent*
        GetAshtonPlayerAbilitySystem(
            AFortPlayerControllerAthena* PlayerController)
    {
        auto PlayerState =
            IsLiveObject(PlayerController) &&
                    PlayerController->HasPlayerState() &&
                    IsLiveObject(
                        PlayerController->PlayerState)
                ? static_cast<AFortPlayerStateAthena*>(
                      PlayerController->PlayerState)
                : nullptr;
        return PlayerState &&
                PlayerState
                    ->HasAbilitySystemComponent() &&
                IsLiveObject(
                    PlayerState
                        ->AbilitySystemComponent)
            ? PlayerState->AbilitySystemComponent
            : nullptr;
    }

    UFortGameplayAbility*
        FindAshtonPassiveInstance(
            AFortPlayerControllerAthena* PlayerController,
            bool bRequireActive = true)
    {
        auto AbilitySystemComponent =
            GetAshtonPlayerAbilitySystem(
                PlayerController);
        auto PassiveClass = FindObject<UClass>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"CarminePack/GA_CarminePack_PassiveSetup."
            L"GA_CarminePack_PassiveSetup_C");
        if (!PassiveClass)
        {
            PassiveClass = FindObject<UClass>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/GA_AshtonPack_PassiveSetup."
            L"GA_AshtonPack_PassiveSetup_C");
        }
        if (!AbilitySystemComponent ||
            !PassiveClass ||
            !AbilitySystemComponent
                 ->HasActivatableAbilities())
        {
            return nullptr;
        }

        auto MatchesOwner =
            [&](UFortGameplayAbility* Ability)
            {
                return IsLiveObject(Ability) &&
                    !Ability->IsDefaultObject() &&
                    Ability->IsA(PassiveClass) &&
                    Ability
                            ->GetAbilitySystemComponentFromActorInfo() ==
                        AbilitySystemComponent;
            };
        auto& Specs =
            AbilitySystemComponent
                ->ActivatableAbilities.Items;
        if (!IsSaneArray(
                Specs.Num(), Specs.Max(), 512) ||
            !IsReadableArrayStorage(
                Specs.Data,
                Specs.Num(),
                static_cast<size_t>(
                    FGameplayAbilitySpec::Size())))
        {
            return nullptr;
        }

        for (int32 SpecIndex = 0;
             SpecIndex < Specs.Num();
             ++SpecIndex)
        {
            auto& Spec = Specs.Get(
                SpecIndex,
                FGameplayAbilitySpec::Size());
            if (!IsLiveObject(Spec.Ability) ||
                !Spec.Ability->IsA(PassiveClass) ||
                (bRequireActive &&
                 Spec.HasActiveCount() &&
                 Spec.ActiveCount == 0))
            {
                continue;
            }
            if (MatchesOwner(Spec.Ability))
                return Spec.Ability;

            auto FindInInstances =
                [&](TArray<UFortGameplayAbility*>&
                        Instances)
                    -> UFortGameplayAbility*
                {
                    if (!IsSaneArray(
                            Instances.Num(),
                            Instances.Max(),
                            16) ||
                        !IsReadableArrayStorage(
                            Instances.Data,
                            Instances.Num(),
                            sizeof(
                                UFortGameplayAbility*)))
                    {
                        return nullptr;
                    }
                    for (int32 InstanceIndex = 0;
                         InstanceIndex <
                             Instances.Num();
                         ++InstanceIndex)
                    {
                        auto Instance =
                            Instances.Get(
                                InstanceIndex);
                        if (MatchesOwner(Instance))
                            return Instance;
                    }
                    return nullptr;
                };
            if (Spec.HasReplicatedInstances())
            {
                if (auto Instance =
                        FindInInstances(
                            Spec.ReplicatedInstances))
                {
                    return Instance;
                }
            }
            if (Spec.HasNonReplicatedInstances())
            {
                if (auto Instance =
                        FindInInstances(
                            Spec.NonReplicatedInstances))
                {
                    return Instance;
                }
            }
        }
        return nullptr;
    }

    UFortGameplayAbility*
        FindAshtonMiloHealthPassiveInstance(
            AFortPlayerControllerAthena* PlayerController)
    {
        auto AbilitySystemComponent =
            GetAshtonPlayerAbilitySystem(
                PlayerController);
        auto PassiveClass = FindObject<UClass>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/Milo/"
            L"GA_Ashton_Milo_Pickup_Passive_HP_Shield."
            L"GA_Ashton_Milo_Pickup_Passive_HP_Shield_C");
        if (!AbilitySystemComponent ||
            !PassiveClass ||
            !AbilitySystemComponent
                 ->HasActivatableAbilities())
        {
            return nullptr;
        }

        auto MatchesOwner =
            [&](UFortGameplayAbility* Ability)
            {
                return IsLiveObject(Ability) &&
                    !Ability->IsDefaultObject() &&
                    Ability->IsA(PassiveClass) &&
                    Ability
                            ->GetAbilitySystemComponentFromActorInfo() ==
                        AbilitySystemComponent;
            };
        auto& Specs =
            AbilitySystemComponent
                ->ActivatableAbilities.Items;
        if (!IsSaneArray(
                Specs.Num(), Specs.Max(), 512) ||
            !IsReadableArrayStorage(
                Specs.Data,
                Specs.Num(),
                static_cast<size_t>(
                    FGameplayAbilitySpec::Size())))
        {
            return nullptr;
        }

        for (int32 SpecIndex = 0;
             SpecIndex < Specs.Num();
             ++SpecIndex)
        {
            auto& Spec = Specs.Get(
                SpecIndex,
                FGameplayAbilitySpec::Size());
            if (!IsLiveObject(Spec.Ability) ||
                !Spec.Ability->IsA(PassiveClass) ||
                (Spec.HasActiveCount() &&
                 Spec.ActiveCount == 0))
            {
                continue;
            }
            if (MatchesOwner(Spec.Ability))
                return Spec.Ability;

            auto FindInInstances =
                [&](TArray<UFortGameplayAbility*>&
                        Instances)
                    -> UFortGameplayAbility*
                {
                    if (!IsSaneArray(
                            Instances.Num(),
                            Instances.Max(),
                            16) ||
                        !IsReadableArrayStorage(
                            Instances.Data,
                            Instances.Num(),
                            sizeof(
                                UFortGameplayAbility*)))
                    {
                        return nullptr;
                    }
                    for (int32 InstanceIndex = 0;
                         InstanceIndex <
                             Instances.Num();
                         ++InstanceIndex)
                    {
                        auto Instance =
                            Instances.Get(
                                InstanceIndex);
                        if (MatchesOwner(Instance))
                            return Instance;
                    }
                    return nullptr;
                };
            if (Spec.HasReplicatedInstances())
            {
                if (auto Instance =
                        FindInInstances(
                            Spec.ReplicatedInstances))
                {
                    return Instance;
                }
            }
            if (Spec.HasNonReplicatedInstances())
            {
                if (auto Instance =
                        FindInInstances(
                            Spec.NonReplicatedInstances))
                {
                    return Instance;
                }
            }
        }
        return nullptr;
    }

    bool InvokeAshtonPassiveTimer(
        AFortPlayerControllerAthena* PlayerController,
        const char* TimerName)
    {
        auto Passive =
            FindAshtonPassiveInstance(
                PlayerController);
        auto Timer =
            Passive && TimerName
                ? Passive->GetFunction(TimerName)
                : nullptr;
        if (!Timer)
            return false;
        Passive->Call<void>(Timer);
        return true;
    }

    uint64 GetCarminePassiveIdentity(
        const UObject* Passive)
    {
        if (!IsLiveObject(Passive))
            return 0;

        FWeakObjectPtr WeakPassive(Passive);
        if (WeakPassive.ObjectIndex < 0 ||
            WeakPassive.ObjectSerialNumber == 0)
        {
            return 0;
        }

        return (static_cast<uint64>(
                    static_cast<uint32>(
                        WeakPassive.ObjectIndex))
                << 32) |
            static_cast<uint32>(
                WeakPassive.ObjectSerialNumber);
    }

    void CarminePassiveReadyOnce(
        UObject* Context,
        FFrame& Stack)
    {
        GCarminePassiveReadyExecutedThisCall = false;
        const uint64 Identity =
            GetCarminePassiveIdentity(Context);
        if (Identity != 0 &&
            !GCarminePassiveReadyExecutions
                 .insert(Identity).second)
        {
            // A manually recovered WaitAnimBPOverrideReady task can still
            // finish later. Consume that empty callback without replaying the
            // montage, camera effects or authored movement timers.
            Stack.IncrementCode();
            SDK::DbgLog(
                "[Ashton1040] suppressed duplicate "
                "Carmine AnimBP-ready callback passive=%p\n",
                static_cast<void*>(Context));
            return;
        }

        if (GCarminePassiveReadyOG)
        {
            GCarminePassiveReadyExecutedThisCall = true;
            GCarminePassiveReadyOG(Context, Stack);
            return;
        }

        Stack.IncrementCode();
    }

    void CarminePassiveTeleportOnce(
        UObject* Context,
        FFrame& Stack)
    {
        const uint64 Identity =
            GetCarminePassiveIdentity(Context);
        if (Identity != 0 &&
            !GCarminePassiveTeleportExecutions
                 .insert(Identity).second)
        {
            Stack.IncrementCode();
            SDK::DbgLog(
                "[Ashton1040] suppressed duplicate "
                "Carmine lift callback passive=%p\n",
                static_cast<void*>(Context));
            return;
        }

        if (GCarminePassiveTeleportOG)
        {
            GCarminePassiveTeleportOG(Context, Stack);
            return;
        }
        Stack.IncrementCode();
    }

    void CarminePassiveSkydiveOnce(
        UObject* Context,
        FFrame& Stack)
    {
        const uint64 Identity =
            GetCarminePassiveIdentity(Context);
        if (Identity != 0 &&
            !GCarminePassiveSkydiveExecutions
                 .insert(Identity).second)
        {
            Stack.IncrementCode();
            SDK::DbgLog(
                "[Ashton1040] suppressed duplicate "
                "Carmine skydive callback passive=%p\n",
                static_cast<void*>(Context));
            return;
        }

        if (GCarminePassiveSkydiveOG)
        {
            GCarminePassiveSkydiveOG(Context, Stack);
            return;
        }
        Stack.IncrementCode();
    }

    using FCarminePassiveExec =
        void (*)(UObject*, FFrame&);

    bool EnsureCarminePassiveCallbackHook(
        UFunction* Function,
        FCarminePassiveExec Detour,
        FCarminePassiveExec& Original)
    {
        if (!Function ||
            !Function->ExecFunction ||
            !Detour)
        {
            return false;
        }

        void* HookAddress =
            reinterpret_cast<void*>(Detour);
        if (Function->ExecFunction != HookAddress)
        {
            Utils::ExecHook(
                Function,
                reinterpret_cast<void*>(Detour),
                Original);
        }
        return Original &&
            Function->ExecFunction == HookAddress;
    }

    bool EnsureCarminePassiveReadyOneShot(
        UFunction* Ready)
    {
        return EnsureCarminePassiveCallbackHook(
            Ready,
            CarminePassiveReadyOnce,
            GCarminePassiveReadyOG);
    }

    bool EnsureCarminePassiveTransitionHooks(
        const UObject* Passive)
    {
        if (!IsLiveObject(Passive))
            return false;

        const bool bReadyInstalled =
            EnsureCarminePassiveReadyOneShot(
                Passive->GetFunction(
                    "OnReady_1EE8BD1E450E43A4918975AD52516282"));
        const bool bTeleportInstalled =
            EnsureCarminePassiveCallbackHook(
                Passive->GetFunction(
                    "TeleportTimer"),
                CarminePassiveTeleportOnce,
                GCarminePassiveTeleportOG);
        const bool bSkydiveInstalled =
            EnsureCarminePassiveCallbackHook(
                Passive->GetFunction(
                    "SkydiveTimer"),
                CarminePassiveSkydiveOnce,
                GCarminePassiveSkydiveOG);
        return bReadyInstalled &&
            bTeleportInstalled &&
            bSkydiveInstalled;
    }

    void PrepareCarminePassiveReadyActivation(
        AFortPlayerControllerAthena* PlayerController)
    {
        auto Passive =
            FindAshtonPassiveInstance(
                PlayerController, false);
        auto HookSource =
            static_cast<UObject*>(Passive);
        if (!HookSource)
        {
            auto PassiveClass =
                FindObject<UClass>(
                    L"/Game/Athena/Items/Gameplay/"
                    L"BackPacks/CarminePack/"
                    L"GA_CarminePack_PassiveSetup."
                    L"GA_CarminePack_PassiveSetup_C");
            HookSource =
                PassiveClass
                    ? PassiveClass->GetDefaultObj()
                    : nullptr;
        }
        if (!HookSource ||
            !EnsureCarminePassiveTransitionHooks(
                HookSource))
        {
            return;
        }
        const uint64 Identity =
            GetCarminePassiveIdentity(Passive);
        if (Identity != 0)
        {
            GCarminePassiveReadyExecutions.erase(
                Identity);
            GCarminePassiveTeleportExecutions.erase(
                Identity);
            GCarminePassiveSkydiveExecutions.erase(
                Identity);
        }
    }

    bool InvokeCarminePassiveReady(
        AFortPlayerControllerAthena* PlayerController)
    {
        auto Passive =
            FindAshtonPassiveInstance(
                PlayerController);
        auto Ready =
            Passive
                ? Passive->GetFunction(
                      "OnReady_1EE8BD1E450E43A4918975AD52516282")
                : nullptr;
        if (!Ready ||
            !EnsureCarminePassiveReadyOneShot(
                Ready))
            return false;

        // This is the cooked WaitAnimBPOverrideReady completion body. It
        // starts Spawn_Montage, the Carmine camera/aura and both authored
        // timers as one sequence. The one-shot hook above consumes the
        // original task's eventual late callback.
        GCarminePassiveReadyExecutedThisCall = false;
        Passive->Call<void>(Ready);
        return GCarminePassiveReadyExecutedThisCall;
    }

    bool IsAshtonPassiveTimerActive(
        AFortPlayerControllerAthena* PlayerController,
        const wchar_t* TimerName,
        bool& bSupported)
    {
        bSupported = false;
        auto Passive =
            FindAshtonPassiveInstance(
                PlayerController);
        auto KismetSystemLibrary =
            UKismetSystemLibrary::GetDefaultObj();
        auto Query =
            KismetSystemLibrary
                ? KismetSystemLibrary->GetFunction(
                      "K2_IsTimerActive")
                : nullptr;
        if (!Passive || !Query || !TimerName)
            return false;

        bSupported = true;
        FString FunctionName(TimerName);
        const bool bActive =
            KismetSystemLibrary->Call<bool>(
            Query, Passive, FunctionName);
        FunctionName.Free();
        return bActive;
    }

    bool ClearAshtonPassiveTimer(
        AFortPlayerControllerAthena* PlayerController,
        const wchar_t* TimerName)
    {
        auto Passive =
            FindAshtonPassiveInstance(
                PlayerController);
        auto KismetSystemLibrary =
            UKismetSystemLibrary::GetDefaultObj();
        auto Clear =
            KismetSystemLibrary
                ? KismetSystemLibrary->GetFunction(
                      "K2_ClearTimer")
                : nullptr;
        if (!Passive || !Clear || !TimerName)
            return false;

        FString FunctionName(TimerName);
        KismetSystemLibrary->Call<void>(
            Clear, Passive, FunctionName);
        FunctionName.Free();
        return true;
    }

    bool VerifyAshtonPassiveTimerInactive(
        AFortPlayerControllerAthena* PlayerController,
        const wchar_t* TimerName,
        bool bWasActive,
        bool bQuerySupported)
    {
        if (!bQuerySupported)
            return false;
        if (!bWasActive)
            return true;
        if (!ClearAshtonPassiveTimer(
                PlayerController, TimerName))
        {
            return false;
        }

        bool bVerifySupported = false;
        const bool bStillActive =
            IsAshtonPassiveTimerActive(
                PlayerController,
                TimerName,
                bVerifySupported);
        return bVerifySupported && !bStillActive;
    }

    void ConsumeCarminePassiveTimerCallback(
        AFortPlayerControllerAthena* PlayerController,
        bool bSkydive)
    {
        auto Passive =
            FindAshtonPassiveInstance(
                PlayerController, false);
        const uint64 Identity =
            GetCarminePassiveIdentity(Passive);
        if (Identity == 0)
            return;

        (bSkydive
             ? GCarminePassiveSkydiveExecutions
             : GCarminePassiveTeleportExecutions)
            .insert(Identity);
    }

    void BeginCarminePromotionSkydiveFallback(
        AFortPlayerControllerAthena* PlayerController,
        AFortPlayerPawnAthena* Pawn)
    {
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(Pawn))
        {
            return;
        }

        // Make a still-scheduled authored callback harmless before applying
        // the bounded movement fallback.
        ConsumeCarminePassiveTimerCallback(
            PlayerController, true);

        if (Pawn->GetFunction("BeginSkydiving"))
            Pawn->BeginSkydiving(true);
        if ((!Pawn->HasbIsSkydiving() ||
             !Pawn->bIsSkydiving) &&
            Pawn->CharacterMovement &&
            Pawn->CharacterMovement->GetFunction(
                "SetMovementMode"))
        {
            Pawn->CharacterMovement->SetMovementMode(
                EMovementMode::MOVE_Falling, 0);
        }

        auto AbilitySystemComponent =
            GetAshtonPlayerAbilitySystem(
                PlayerController);
        auto SkydiveEffectClass =
            FindObject<UClass>(
                L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Skydive."
                L"GE_Carmine_GC_Skydive_C");
        if (AbilitySystemComponent &&
            SkydiveEffectClass)
        {
            auto Context =
                AbilitySystemComponent
                    ->MakeEffectContext();
            Context.Instigator =
                PlayerController;
            Context.Causer = Pawn;
            Context.AddSourceObject(Pawn);
            AbilitySystemComponent
                ->BP_ApplyGameplayEffectToSelf(
                    SkydiveEffectClass,
                    1.0f,
                    Context);
        }
        Pawn->ForceNetUpdate();
        PlayerController->ForceNetUpdate();
    }

    int32 RemoveAshtonRuntimeEffect(
        UAbilitySystemComponent* AbilitySystemComponent,
        const wchar_t* EffectClassName)
    {
        if (!AbilitySystemComponent ||
            !EffectClassName ||
            !AbilitySystemComponent
                 ->HasActiveGameplayEffects())
        {
            return 0;
        }
        auto& Effects =
            AbilitySystemComponent
                ->ActiveGameplayEffects
                .GameplayEffects_Internal;
        if (!IsSaneArray(
                Effects.Num(), Effects.Max(), 2048) ||
            !IsReadableArrayStorage(
                Effects.Data,
                Effects.Num(),
                static_cast<size_t>(
                    FActiveGameplayEffect::Size())))
        {
            return 0;
        }

        std::vector<FActiveGameplayEffectHandle>
            Handles;
        for (int32 Index = 0;
             Index < Effects.Num();
             ++Index)
        {
            auto& Effect = Effects.Get(
                Index,
                FActiveGameplayEffect::Size());
            if (!IsLiveObject(Effect.Spec.Def) ||
                !IsLiveObject(Effect.Spec.Def->Class) ||
                Effect.Spec.Def->Class->Name.ToWString() !=
                    EffectClassName)
            {
                continue;
            }
            FActiveGameplayEffectHandle Handle{};
            memcpy(
                &Handle,
                reinterpret_cast<const uint8*>(
                    &Effect) + 0xC,
                sizeof(Handle));
            if (Handle.Handle > 0)
                Handles.push_back(Handle);
        }

        auto RemoveEffect =
            AbilitySystemComponent->GetFunction(
                "RemoveActiveGameplayEffect");
        int32 Removed = 0;
        if (RemoveEffect)
        {
            for (auto& Handle : Handles)
            {
                if (AbilitySystemComponent->Call<bool>(
                        RemoveEffect,
                        Handle,
                        -1))
                {
                    ++Removed;
                }
            }
        }
        return Removed;
    }

    bool ApplyAshtonCarmineHealthPair(
        UAbilitySystemComponent* AbilitySystemComponent,
        AFortPlayerControllerAthena* PlayerController,
        AFortPlayerPawnAthena* Pawn,
        bool bRealityStoneCaptured)
    {
        if (!AbilitySystemComponent ||
            !IsLiveObject(PlayerController) ||
            !IsLiveObject(Pawn))
        {
            return false;
        }

        const wchar_t* MaxEffectPath =
            bRealityStoneCaptured
                ? L"/Game/Athena/Items/Gameplay/BackPacks/"
                  L"CarminePack/GE_Carmine_Health_Red."
                  L"GE_Carmine_Health_Red_C"
                : L"/Game/Athena/Items/Gameplay/BackPacks/"
                  L"CarminePack/GE_Carmine_Health."
                  L"GE_Carmine_Health_C";
        const wchar_t* FillEffectPath =
            bRealityStoneCaptured
                ? L"/Game/Athena/Items/Gameplay/BackPacks/"
                  L"CarminePack/GE_Carmine_StartingShields_Red."
                  L"GE_Carmine_StartingShields_Red_C"
                : L"/Game/Athena/Items/Gameplay/BackPacks/"
                  L"CarminePack/GE_Carmine_StartingShields."
                  L"GE_Carmine_StartingShields_C";
        auto MaxEffect =
            FindObject<UClass>(MaxEffectPath);
        auto FillEffect =
            FindObject<UClass>(FillEffectPath);
        if (!MaxEffect || !FillEffect)
            return false;

        auto Context =
            AbilitySystemComponent->MakeEffectContext();
        Context.Instigator = PlayerController;
        Context.Causer = Pawn;
        Context.AddSourceObject(Pawn);
        // Carmine's composite curves are authored at time/level zero. Level
        // one evaluates these exact 10.40 health effects to zero.
        AbilitySystemComponent
            ->BP_ApplyGameplayEffectToSelf(
                MaxEffect, 0.0f, Context);
        AbilitySystemComponent
            ->BP_ApplyGameplayEffectToSelf(
                FillEffect, 0.0f, Context);
        return true;
    }

    void ReconcileAshtonStoneHealthEffects(
        AFortGameStateAthena* GameState,
        AFortAthenaMutator_Ashton* Ashton,
        const char* Reason)
    {
        if (!IsLiveObject(GameState) ||
            !IsLiveObject(Ashton) ||
            !HasInitializedAshtonStoneRuntime(Ashton) ||
            !GameState->HasPlayerArray())
        {
            return;
        }

        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        const int32 DesiredStoneCount =
            (std::min)(
                6,
                (std::max)(
                    0,
                    CountCapturedAshtonStones(
                        Ashton)));
        auto RealityStone =
            FindAshtonStoneState(Ashton, 2);
        const bool bRealityStoneCaptured =
            PhaseState
                .bAshtonStoneCaptureLocked[2] ||
            (RealityStone &&
             RealityStone->StoneState ==
                 AshtonStoneStateCaptured);
        auto PublishedLeader =
            !PhaseState.bAshtonLeaderVacant &&
                    Ashton->HasVillainLeaderPC() &&
                    IsValidAshtonLeaderController(
                        Ashton->VillainLeaderPC)
                ? Ashton->VillainLeaderPC
                : nullptr;

        auto& Players = GameState->PlayerArray;
        if (!IsSaneArray(
                Players.Num(), Players.Max(), 256) ||
            !IsReadableArrayStorage(
                Players.Data,
                Players.Num(),
                sizeof(AFortPlayerStateAthena*)))
        {
            return;
        }

        for (auto PlayerState : Players)
        {
            auto PlayerController =
                IsLiveObject(PlayerState) &&
                        PlayerState->HasOwner() &&
                        IsLiveObject(PlayerState->Owner)
                    ? PlayerState->Owner
                          ->Cast<
                              AFortPlayerControllerAthena>()
                    : nullptr;
            if (!IsLiveObject(PlayerController))
                continue;

            auto AbilitySystemComponent =
                GetAshtonPlayerAbilitySystem(
                    PlayerController);
            if (!AbilitySystemComponent)
                continue;

            const bool bVillain =
                PlayerState->HasTeamIndex() &&
                PlayerState->TeamIndex ==
                    PhaseState.AshtonVillainTeam;
            const bool bLeader =
                bVillain &&
                PublishedLeader ==
                    PlayerController;
            auto Pawn =
                PlayerController->HasMyFortPawn() &&
                        IsLiveObject(
                            PlayerController->MyFortPawn)
                    ? PlayerController->MyFortPawn
                    : nullptr;
            if (!bVillain || bLeader)
            {
                const int32 RemovedPickupEffects =
                    RemoveAshtonRuntimeEffect(
                        AbilitySystemComponent,
                        L"GE_AshtonPack_Set_HP-Shield_"
                        L"Milo_Pickup_C");
                const int32 RemovedBaseEffects =
                    RemoveAshtonRuntimeEffect(
                        AbilitySystemComponent,
                        L"GE_AshtonPack_Set_HP-Shield_"
                        L"Milo_C");
                PhaseState
                    .AshtonMiloHealthReconcileStates
                    .erase(PlayerController);
                if (RemovedPickupEffects > 0 ||
                    RemovedBaseEffects > 0)
                {
                    SDK::DbgLog(
                        "[Ashton1040] cleared Milo health "
                        "effects PC=%p leader=%d pickup=%d "
                        "base=%d reason=%s\n",
                        static_cast<void*>(
                            PlayerController),
                        bLeader ? 1 : 0,
                        RemovedPickupEffects,
                        RemovedBaseEffects,
                        Reason ? Reason : "unknown");
                }
            }

            if (!bVillain)
            {
                PhaseState
                    .AshtonCarmineHealthReconcileStates
                    .erase(PlayerController);
                continue;
            }

            if (!Pawn ||
                Pawn->GetHealth() <= 0.0f)
            {
                continue;
            }

            if (bLeader)
            {
                FGuid LeaderGadgetGuid{};
                const auto LeaderItem =
                    PhaseState
                        .AshtonInitializedLeaderItems
                        .find(PlayerController);
                if (LeaderItem !=
                    PhaseState
                        .AshtonInitializedLeaderItems.end())
                {
                    LeaderGadgetGuid =
                        LeaderItem->second;
                }

                auto& Tracked =
                    PhaseState
                        .AshtonCarmineHealthReconcileStates
                        [PlayerController];
                const uint64 PawnIdentity =
                    GetCarminePassiveIdentity(Pawn);
                const bool bNewGeneration =
                    !Tracked.bApplied ||
                    Tracked.PawnIdentity !=
                        PawnIdentity ||
                    Tracked.GadgetGuid !=
                        LeaderGadgetGuid ||
                    Tracked.bRealityStoneCaptured !=
                        bRealityStoneCaptured;
                const float DesiredValue =
                    bRealityStoneCaptured
                        ? 2000.0f
                        : 1000.0f;
                const float PreviousMaxHealth =
                    Pawn->GetMaxHealth();
                const float PreviousMaxShield =
                    Pawn->GetMaxShield();
                const float PreviousHealth =
                    Pawn->GetHealth();
                const float PreviousShield =
                    Pawn->GetShield();
                const bool bMaxMismatch =
                    std::abs(
                        PreviousMaxHealth -
                        DesiredValue) > 0.5f ||
                    std::abs(
                        PreviousMaxShield -
                        DesiredValue) > 0.5f;
                bool bAppliedAuthoredPair = false;
                if (bNewGeneration)
                {
                    bAppliedAuthoredPair =
                        ApplyAshtonCarmineHealthPair(
                            AbilitySystemComponent,
                            PlayerController,
                            Pawn,
                            bRealityStoneCaptured);
                }

                if (bNewGeneration || bMaxMismatch)
                {
                    Pawn->SetMaxHealth(DesiredValue);
                    Pawn->SetMaxShield(DesiredValue);

                    // Fill once for a new Thanos generation and once for the
                    // Reality-stone upgrade. If a late legacy health effect
                    // overwrites only the maxima, preserve damage unless the
                    // player was still full at that old cap.
                    const bool bHealthWasFull =
                        PreviousMaxHealth > 0.0f &&
                        PreviousHealth >=
                            PreviousMaxHealth - 0.5f;
                    const bool bShieldWasFull =
                        PreviousMaxShield > 0.0f &&
                        PreviousShield >=
                            PreviousMaxShield - 0.5f;
                    if (bNewGeneration ||
                        (bMaxMismatch &&
                         bHealthWasFull))
                    {
                        Pawn->SetHealth(
                            DesiredValue);
                    }
                    else if (PreviousHealth >
                             DesiredValue)
                    {
                        Pawn->SetHealth(
                            DesiredValue);
                    }
                    if (bNewGeneration ||
                        (bMaxMismatch &&
                         bShieldWasFull))
                    {
                        Pawn->SetShield(
                            DesiredValue);
                    }
                    else if (PreviousShield >
                             DesiredValue)
                    {
                        Pawn->SetShield(
                            DesiredValue);
                    }

                    Pawn->ForceNetUpdate();
                    PlayerState->ForceNetUpdate();
                    PlayerController->ForceNetUpdate();
                    SDK::DbgLog(
                        "[Ashton1040] reconciled authored "
                        "Carmine health PC=%p value=%.0f "
                        "reality=%d newGeneration=%d "
                        "maxMismatch=%d effectPair=%d "
                        "reason=%s\n",
                        static_cast<void*>(
                            PlayerController),
                        DesiredValue,
                        bRealityStoneCaptured ? 1 : 0,
                        bNewGeneration ? 1 : 0,
                        bMaxMismatch ? 1 : 0,
                        bAppliedAuthoredPair ? 1 : 0,
                        Reason ? Reason : "unknown");
                }

                Tracked.PawnIdentity =
                    PawnIdentity;
                Tracked.GadgetGuid =
                    LeaderGadgetGuid;
                Tracked.bRealityStoneCaptured =
                    bRealityStoneCaptured;
                Tracked.bApplied = true;
                continue;
            }

            PhaseState
                .AshtonCarmineHealthReconcileStates
                .erase(PlayerController);

            FGuid MainGadgetGuid{};
            bool bInitializedMainGadget = false;
            const auto InitializedItems =
                PhaseState
                    .AshtonInitializedVillainItems
                    .find(PlayerController);
            if (InitializedItems !=
                    PhaseState
                        .AshtonInitializedVillainItems.end() &&
                IsLiveObject(
                    PlayerController->WorldInventory))
            {
                for (const auto&
                         [Definition, Guid] :
                     InitializedItems->second)
                {
                    if (!IsLiveObject(Definition) ||
                        Definition->Name.ToWString() !=
                            L"AGID_AshtonPack_Milo")
                    {
                        continue;
                    }
                    auto Existing =
                        PlayerController
                            ->WorldInventory
                            ->Inventory
                            .ReplicatedEntries.Search(
                                [&](FFortItemEntry& Entry)
                                {
                                    return Entry
                                                .ItemDefinition ==
                                            Definition &&
                                        Entry.ItemGuid.A ==
                                            Guid.A &&
                                        Entry.ItemGuid.B ==
                                            Guid.B &&
                                        Entry.ItemGuid.C ==
                                            Guid.C &&
                                        Entry.ItemGuid.D ==
                                            Guid.D;
                                },
                                FFortItemEntry::Size());
                    if (Existing)
                    {
                        MainGadgetGuid = Guid;
                        bInitializedMainGadget = true;
                    }
                    break;
                }
            }
            if (!bInitializedMainGadget)
                continue;

            auto Passive =
                FindAshtonMiloHealthPassiveInstance(
                    PlayerController);
            auto ApplyEffects =
                Passive
                    ? Passive->GetFunction(
                          "ApplyEffects")
                    : nullptr;
            const uint64 PassiveIdentity =
                GetCarminePassiveIdentity(Passive);
            if (!ApplyEffects ||
                PassiveIdentity == 0)
            {
                continue;
            }

            auto& Tracked =
                PhaseState
                    .AshtonMiloHealthReconcileStates
                    [PlayerController];
            const bool bNewPassiveOrGadget =
                Tracked.PassiveIdentity !=
                    PassiveIdentity ||
                Tracked.GadgetGuid !=
                    MainGadgetGuid;
            int32 RemovedStaleEffects = 0;
            if (bNewPassiveOrGadget)
            {
                // The authored passive stores its own active-effect handle.
                // Reset a PlayerState-retained stack exactly once for each new
                // passive/gadget generation, then let ApplyEffects rebuild and
                // retain the authoritative handle. Later native tag callbacks
                // will update that same stack instead of creating a second one.
                RemovedStaleEffects =
                    RemoveAshtonRuntimeEffect(
                        AbilitySystemComponent,
                        L"GE_AshtonPack_Set_HP-Shield_"
                        L"Milo_Pickup_C");
                Tracked.PassiveIdentity =
                    PassiveIdentity;
                Tracked.GadgetGuid =
                    MainGadgetGuid;
                Tracked.AppliedStoneCount = -1;
            }

            if (Tracked.AppliedStoneCount ==
                DesiredStoneCount)
            {
                continue;
            }

            Passive->Call<void>(
                ApplyEffects,
                DesiredStoneCount);
            Tracked.AppliedStoneCount =
                DesiredStoneCount;
            Pawn->ForceNetUpdate();
            PlayerState->ForceNetUpdate();
            PlayerController->ForceNetUpdate();
            SDK::DbgLog(
                "[Ashton1040] reconciled authored Milo "
                "stone health PC=%p stacks=%d "
                "newPassive=%d staleEffects=%d "
                "reason=%s\n",
                static_cast<void*>(
                    PlayerController),
                DesiredStoneCount,
                bNewPassiveOrGadget ? 1 : 0,
                RemovedStaleEffects,
                Reason ? Reason : "unknown");
        }
    }

    void ResetAshtonPromotionRuntime(
        AFortPlayerControllerAthena* PlayerController,
        bool bStartSkydiving)
    {
        if (!IsLiveObject(PlayerController))
            return;

        auto Passive =
            FindAshtonPassiveInstance(
                PlayerController);
        if (Passive)
        {
            if (auto ClearCamera =
                    Passive->GetFunction(
                        "ClearCameraMode"))
            {
                Passive->Call<void>(
                    ClearCamera);
            }
        }

        if (auto SetIgnoreLook =
                PlayerController->GetFunction(
                    "SetIgnoreLookInput"))
        {
            PlayerController->Call<void>(
                SetIgnoreLook, false);
        }
        if (auto SetIgnoreMove =
                PlayerController->GetFunction(
                    "SetIgnoreMoveInput"))
        {
            PlayerController->Call<void>(
                SetIgnoreMove, false);
        }
        if (PlayerController->GetFunction(
                "ClientIgnoreLookInput"))
        {
            PlayerController
                ->ClientIgnoreLookInput(false);
        }
        if (PlayerController->GetFunction(
                "ClientIgnoreMoveInput"))
        {
            PlayerController
                ->ClientIgnoreMoveInput(false);
        }

        auto AbilitySystemComponent =
            GetAshtonPlayerAbilitySystem(
                PlayerController);
        static constexpr const wchar_t*
            TransitionEffects[] = {
                L"GE_Carmine_DamageImmune_C",
                L"GE_Carmine_AbilityBlocker_C",
                L"GE_AshtonPack_DamageImmune_C",
                L"GE_AshtonPack_AbilityBlocker_C"
            };
        for (const auto Effect :
             TransitionEffects)
        {
            RemoveAshtonRuntimeEffect(
                AbilitySystemComponent,
                Effect);
        }
        if (!bStartSkydiving)
        {
            static constexpr const wchar_t*
                PersistentLeaderEffects[] = {
                    L"GE_Carmine_Speed_C",
                    L"GE_Carmine_FallDamageImmune_C",
                    L"GE_Carmine_FloorTrapImmune_C",
                    L"GE_Carmine_DisableCrouch_C",
                    L"GE_Carmine_Equipped_C",
                    L"GE_Carmine_DisableDBNO_C",
                    L"GE_Carmine_GC_Aura_C",
                    L"GE_Carmine_GC_Skydive_C",
                    L"GE_AshtonPack_GC_Aura_C",
                    L"GE_AshtonPack_GC_Skydive_C",
                    L"GE_Carmine_GC_Jump_Trails_C",
                    L"GE_Carmine_GC_Beam_Loop_C",
                    L"GE_Ashton_Carmine_LockInPlace_C",
                    L"GE_Ashton_Carmine_GemPickUpAnim_C",
                    L"GE_Ashton_Carmine_"
                    L"FinalGemPickUpAnim_C"
                };
            for (const auto Effect :
                 PersistentLeaderEffects)
            {
                RemoveAshtonRuntimeEffect(
                    AbilitySystemComponent,
                    Effect);
            }
        }

        auto Pawn =
            PlayerController->HasMyFortPawn() &&
                    IsLiveObject(
                        PlayerController->MyFortPawn)
                ? PlayerController->MyFortPawn
                : nullptr;
        if (!Pawn)
            return;
        if (bStartSkydiving &&
            Pawn->GetFunction("BeginSkydiving"))
        {
            Pawn->BeginSkydiving(true);
            auto SkydiveEffectClass =
                FindObject<UClass>(
                    L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"Ashton/GE_AshtonPack_GC_Skydive."
                    L"GE_AshtonPack_GC_Skydive_C");
            if (AbilitySystemComponent &&
                SkydiveEffectClass)
            {
                auto Context =
                    AbilitySystemComponent
                        ->MakeEffectContext();
                Context.Instigator =
                    PlayerController;
                Context.Causer = Pawn;
                Context.AddSourceObject(Pawn);
                AbilitySystemComponent
                    ->BP_ApplyGameplayEffectToSelf(
                        SkydiveEffectClass,
                        1.0f,
                        Context);
            }
        }
        else if (Pawn->CharacterMovement &&
                 Pawn->CharacterMovement
                     ->GetFunction(
                         "SetMovementMode"))
        {
            Pawn->CharacterMovement
                ->SetMovementMode(
                    EMovementMode::MOVE_Falling,
                    0);
        }
        Pawn->ForceNetUpdate();
    }

    int32 RemoveAllAshtonLeaderArtifacts(
        AFortAthenaMutator_Ashton* Ashton,
        AFortPlayerControllerAthena* PlayerController)
    {
        auto& State = GAuthoredNativeLTMPhaseState;
        const bool bHadLeaderRuntime =
            State.AshtonInitializedLeaderItems.contains(
                PlayerController) ||
            State.AshtonInitializedLeaderBackingItems
                .contains(PlayerController) ||
            State.AshtonPromotionController ==
                PlayerController;
        State.AshtonInitializedLeaderItems.erase(
            PlayerController);
        State.AshtonInitializedLeaderBackingItems.erase(
            PlayerController);
        State.AshtonCarmineHealthReconcileStates.erase(
            PlayerController);
        CancelAshtonLeaderPromotion(
            PlayerController);
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(PlayerController->WorldInventory))
        {
            if (bHadLeaderRuntime)
            {
                ResetAshtonPromotionRuntime(
                    PlayerController, false);
            }
            return 0;
        }

        auto Inventory =
            PlayerController->WorldInventory;
        auto& Rows =
            Inventory->Inventory.ReplicatedEntries;
        std::vector<FGuid> RowsToRemove;
        if (IsSaneArray(Rows.Num(), Rows.Max(), 256) &&
            IsReadableArrayStorage(
                Rows.Data,
                Rows.Num(),
                static_cast<size_t>(
                    FFortItemEntry::Size())))
        {
            RowsToRemove.reserve(Rows.Num());
            for (int32 Index = 0;
                 Index < Rows.Num();
                 ++Index)
            {
                auto& Entry = Rows.Get(
                    Index, FFortItemEntry::Size());
                if (IsAnyAshtonLeaderArtifact(
                        Ashton,
                        Entry.ItemDefinition))
                {
                    RowsToRemove.push_back(
                        Entry.ItemGuid);
                }
            }
        }

        for (const auto Guid : RowsToRemove)
            Inventory->Remove(Guid);
        const bool bDemoted =
            bHadLeaderRuntime ||
            !RowsToRemove.empty();
        if (bDemoted)
        {
            // End/unequip Carmine first so its two-second Red-stone loop cannot
            // restore the instant 2,000/2,000 overrides after the role reset.
            ResetAshtonPromotionRuntime(
                PlayerController, false);
            auto Pawn =
                PlayerController->HasMyFortPawn() &&
                        IsLiveObject(
                            PlayerController->MyFortPawn)
                    ? PlayerController->MyFortPawn
                    : nullptr;
            const bool bLivingPawn =
                Pawn &&
                Pawn->GetHealth() > 0.0f &&
                (!Pawn->HasbIsDying() ||
                 !Pawn->bIsDying) &&
                (!Pawn->HasbPlayedDying() ||
                 !Pawn->bPlayedDying);
            if (bLivingPawn)
            {
                // Carmine's base health effects are Instant, so removing an
                // active GE cannot undo them. Restore the ordinary Athena
                // baseline; the subsequently granted Milo ability set then
                // applies its authored +25/+25 and captured-stone stacks.
                Pawn->SetMaxHealth(100.0f);
                Pawn->SetMaxShield(100.0f);
                Pawn->SetHealth(100.0f);
                Pawn->SetShield(100.0f);
                Pawn->ForceNetUpdate();
            }
            Inventory->ForceNetUpdate();
            PlayerController->ForceNetUpdate();
        }
        return static_cast<int32>(
            RowsToRemove.size());
    }

    void QueueAshtonLeaderPromotion(
        AFortPlayerControllerAthena* PlayerController,
        const FGuid& ItemGuid)
    {
        UWorld* World = UWorld::GetWorld();
        if (!World ||
            !IsValidAshtonLeaderController(
                PlayerController))
        {
            return;
        }

        auto& State = GAuthoredNativeLTMPhaseState;
        auto Pawn = PlayerController->MyFortPawn;
        State.AshtonPromotionController =
            PlayerController;
        State.AshtonPromotionPawn = Pawn;
        State.AshtonPromotionItemGuid = ItemGuid;
        State.AshtonPromotionStartedAt =
            UGameplayStatics::GetTimeSeconds(World);
        State.AshtonPromotionStartZ =
            Pawn->K2_GetActorLocation().Z;
        State.AshtonPromotionReadyObservedAt = 0.0;
        State.AshtonPromotionReadyFallbackAt = 0.0;
        State.AshtonPromotionTeleportFallbackAt = 0.0;
        State.AshtonPromotionSkydiveFallbackAt = 0.0;
        State.bAshtonPromotionUsesCarmine =
            IsLiveObject(State.AshtonMutator) &&
            State.AshtonMutator
                ->HasVillainLeaderItemDef() &&
            IsLiveObject(
                State.AshtonMutator
                    ->VillainLeaderItemDef) &&
            State.AshtonMutator
                    ->VillainLeaderItemDef
                    ->Name.ToWString() ==
                L"AGID_CarminePack";
        State.bAshtonPromotionReadyFallbackInvoked =
            false;
        State.bAshtonPromotionTeleportFallbackInvoked =
            false;
        State.bAshtonPromotionSkydiveFallbackInvoked =
            false;
        State.bAshtonPromotionTeleported = false;
        State.bAshtonPromotionSkydiving = false;
        SDK::DbgLog(
            "[Ashton1040] queued %s spawn sequence "
            "leader=%p pawn=%p guid=%08X-%08X-%08X-%08X\n",
            State.bAshtonPromotionUsesCarmine
                ? "Carmine observational"
                : "base Ashton compatibility",
            static_cast<void*>(PlayerController),
            static_cast<void*>(Pawn),
            static_cast<unsigned>(ItemGuid.A),
            static_cast<unsigned>(ItemGuid.B),
            static_cast<unsigned>(ItemGuid.C),
            static_cast<unsigned>(ItemGuid.D));
    }

    void TickAshtonLeaderPromotion(double Now)
    {
        auto& State = GAuthoredNativeLTMPhaseState;
        auto PlayerController =
            State.AshtonPromotionController;
        auto Pawn = State.AshtonPromotionPawn;
        if (!PlayerController || !Pawn)
            return;

        const bool bStillLeader =
            !State.bAshtonLeaderVacant &&
            IsLiveObject(State.AshtonMutator) &&
            State.AshtonMutator->HasVillainLeaderPC() &&
            State.AshtonMutator->VillainLeaderPC ==
                PlayerController &&
            IsValidAshtonLeaderController(
                PlayerController) &&
            PlayerController->MyFortPawn == Pawn;
        auto LeaderEntry =
            bStillLeader
                ? PlayerController->WorldInventory
                      ->Inventory.ReplicatedEntries.Search(
                          [&](FFortItemEntry& Entry)
                          {
                              return Entry.ItemGuid ==
                                      State
                                          .AshtonPromotionItemGuid &&
                                  IsAshtonLeaderInventoryDefinition(
                                      State.AshtonMutator,
                                      Entry.ItemDefinition);
                          },
                          FFortItemEntry::Size())
                : nullptr;
        if (!bStillLeader || !LeaderEntry)
        {
            CancelAshtonLeaderPromotion(
                PlayerController);
            return;
        }

        const double Elapsed =
            Now - State.AshtonPromotionStartedAt;
        if (State.bAshtonPromotionUsesCarmine)
        {
            auto CarminePassive =
                FindAshtonPassiveInstance(
                    PlayerController, false);
            if (CarminePassive)
            {
                EnsureCarminePassiveTransitionHooks(
                    CarminePassive);
            }
            const uint64 PassiveIdentity =
                GetCarminePassiveIdentity(
                    CarminePassive);
            if (State
                        .AshtonPromotionReadyObservedAt <=
                    0.0 &&
                PassiveIdentity != 0 &&
                GCarminePassiveReadyExecutions
                    .contains(PassiveIdentity))
            {
                State.AshtonPromotionReadyObservedAt =
                    Now;
                SDK::DbgLog(
                    "[Ashton1040] observed authored "
                    "Carmine AnimBP-ready sequence "
                    "leader=%p passive=%p\n",
                    static_cast<void*>(
                        PlayerController),
                    static_cast<void*>(
                        CarminePassive));
            }

            FVector Location =
                Pawn->K2_GetActorLocation();
            bool bLiftObserved =
                Location.Z >=
                    State.AshtonPromotionStartZ +
                        1500.0;
            if (bLiftObserved &&
                !State.bAshtonPromotionTeleported)
            {
                State.bAshtonPromotionTeleported =
                    true;
                if (State
                        .AshtonPromotionReadyObservedAt <=
                    0.0)
                {
                    // If the callback completed before its hook was installed,
                    // the authored 1-second lift still proves that the body
                    // montage/camera sequence started. Anchor watchdog timing
                    // to that sequence rather than to the earlier item grant.
                    State
                        .AshtonPromotionReadyObservedAt =
                        Now - 1.0;
                }
                SDK::DbgLog(
                    "[Ashton1040] observed authored "
                    "Carmine lift leader=%p z=%.1f\n",
                    static_cast<void*>(
                        PlayerController),
                    Location.Z);
            }

            if (State
                    .AshtonPromotionReadyObservedAt <=
                0.0)
            {
                // Do not bypass WaitAnimBPOverrideReady. Calling the cooked
                // movement timers from gadget-grant time can lift the camera
                // while the Thanos body montage has not become playable yet.
                return;
            }
            const double CarmineElapsed =
                Now -
                State.AshtonPromotionReadyObservedAt;
            if (!State.bAshtonPromotionTeleported &&
                CarmineElapsed >= 1.25)
            {
                if (!State
                         .bAshtonPromotionTeleportFallbackInvoked)
                {
                    bool bTimerQuerySupported = false;
                    const bool bTimerActive =
                        IsAshtonPassiveTimerActive(
                            PlayerController,
                            L"TeleportTimer",
                            bTimerQuerySupported);
                    // The authored timer normally fires at 1.0 seconds. If it
                    // is still registered, allow one extra grace interval
                    // before replacing it so a delayed game-thread tick cannot
                    // produce two 35k lifts.
                    const bool bMayReplaceTimer =
                        !bTimerActive ||
                        CarmineElapsed >= 2.25;
                    const bool bTimerVerifiedInactive =
                        bMayReplaceTimer &&
                        VerifyAshtonPassiveTimerInactive(
                            PlayerController,
                            L"TeleportTimer",
                            bTimerActive,
                            bTimerQuerySupported);
                    if (bTimerVerifiedInactive)
                    {
                        const bool bTimerInvoked =
                            InvokeAshtonPassiveTimer(
                                PlayerController,
                                "TeleportTimer");
                        State
                            .bAshtonPromotionTeleportFallbackInvoked =
                            true;
                        State
                            .AshtonPromotionTeleportFallbackAt =
                            Now;
                        Location =
                            Pawn->K2_GetActorLocation();
                        bLiftObserved =
                            Location.Z >=
                                State
                                    .AshtonPromotionStartZ +
                                    1500.0;
                        State.bAshtonPromotionTeleported =
                            bLiftObserved;
                        SDK::DbgLog(
                            "[Ashton1040] invoked Carmine "
                            "lift watchdog leader=%p "
                            "timerQuery=%d timerActive=%d "
                            "timerInvoked=%d observed=%d "
                            "z=%.1f\n",
                            static_cast<void*>(
                                PlayerController),
                            bTimerQuerySupported ? 1 : 0,
                            bTimerActive ? 1 : 0,
                            bTimerInvoked ? 1 : 0,
                            bLiftObserved ? 1 : 0,
                            Location.Z);
                    }
                    else if (
                        bMayReplaceTimer &&
                        (!bTimerQuerySupported ||
                         CarmineElapsed >= 3.00))
                    {
                        // Cancellation could not be proven. Never call the
                        // authored timer alongside a possibly live callback;
                        // advance to the absolute-height fallback instead.
                        State
                            .bAshtonPromotionTeleportFallbackInvoked =
                            true;
                        State
                            .AshtonPromotionTeleportFallbackAt =
                            Now - 0.75;
                        SDK::DbgLog(
                            "[Ashton1040] Carmine lift timer "
                            "could not be verified inactive "
                            "leader=%p timerQuery=%d "
                            "timerActive=%d; using hard "
                            "fallback\n",
                            static_cast<void*>(
                                PlayerController),
                            bTimerQuerySupported ? 1 : 0,
                            bTimerActive ? 1 : 0);
                    }
                }
                else if (
                    Now >=
                    State
                            .AshtonPromotionTeleportFallbackAt +
                        0.75)
                {
                    // Suppress a timer that survived cancellation before the
                    // absolute-height fallback makes the same transition.
                    ConsumeCarminePassiveTimerCallback(
                        PlayerController, false);
                    bool bTimerQuerySupported = false;
                    if (IsAshtonPassiveTimerActive(
                            PlayerController,
                            L"TeleportTimer",
                            bTimerQuerySupported))
                    {
                        ClearAshtonPassiveTimer(
                            PlayerController,
                            L"TeleportTimer");
                    }
                    Location =
                        Pawn->K2_GetActorLocation();
                    Location.Z =
                        State.AshtonPromotionStartZ +
                        35000.0;
                    Pawn->K2_TeleportTo(
                        Location,
                        Pawn->K2_GetActorRotation());
                    if (Pawn->CharacterMovement &&
                        Pawn->CharacterMovement
                            ->GetFunction(
                                "SetMovementMode"))
                    {
                        Pawn->CharacterMovement
                            ->SetMovementMode(
                                EMovementMode::MOVE_None,
                                0);
                    }
                    Pawn->ForceNetUpdate();
                    State.bAshtonPromotionTeleported =
                        true;
                    SDK::DbgLog(
                        "[Ashton1040] applied exact-height "
                        "Carmine lift fallback leader=%p "
                        "z=%.1f timerQuery=%d\n",
                        static_cast<void*>(
                            PlayerController),
                        Pawn->K2_GetActorLocation().Z,
                        bTimerQuerySupported ? 1 : 0);
                }
            }

            // Never begin the descent until either the native or the bounded
            // exact-height lift has actually been observed.
            if (!State.bAshtonPromotionTeleported)
                return;

            const bool bSkydivingObserved =
                Pawn->HasbIsSkydiving() &&
                Pawn->bIsSkydiving;
            if (bSkydivingObserved)
            {
                State.bAshtonPromotionSkydiving =
                    true;
                SDK::DbgLog(
                    "[Ashton1040] observed authored "
                    "Carmine skydive leader=%p pawn=%p\n",
                    static_cast<void*>(
                        PlayerController),
                    static_cast<void*>(Pawn));
                CancelAshtonLeaderPromotion(
                    PlayerController);
                return;
            }

            const double SkydiveWatchdogAt =
                State.AshtonPromotionReadyObservedAt +
                3.90;
            const double SkydiveTimerReplacementAt =
                State.AshtonPromotionReadyObservedAt +
                4.65;
            if (Now >= SkydiveWatchdogAt)
            {
                if (!State
                         .bAshtonPromotionSkydiveFallbackInvoked)
                {
                    bool bTimerQuerySupported = false;
                    const bool bTimerActive =
                        IsAshtonPassiveTimerActive(
                            PlayerController,
                            L"SkydiveTimer",
                            bTimerQuerySupported);
                    const bool bMayReplaceTimer =
                        !bTimerActive ||
                        Now >=
                            SkydiveTimerReplacementAt;
                    const bool bTimerVerifiedInactive =
                        bMayReplaceTimer &&
                        VerifyAshtonPassiveTimerInactive(
                            PlayerController,
                            L"SkydiveTimer",
                            bTimerActive,
                            bTimerQuerySupported);
                    if (bTimerVerifiedInactive)
                    {
                        const bool bTimerInvoked =
                            InvokeAshtonPassiveTimer(
                                PlayerController,
                                "SkydiveTimer");
                        State
                            .bAshtonPromotionSkydiveFallbackInvoked =
                            true;
                        State
                            .AshtonPromotionSkydiveFallbackAt =
                            Now;
                        const bool bObservedAfterCall =
                            Pawn->HasbIsSkydiving() &&
                            Pawn->bIsSkydiving;
                        SDK::DbgLog(
                            "[Ashton1040] invoked Carmine "
                            "skydive watchdog leader=%p "
                            "timerQuery=%d timerActive=%d "
                            "timerInvoked=%d observed=%d\n",
                            static_cast<void*>(
                                PlayerController),
                            bTimerQuerySupported ? 1 : 0,
                            bTimerActive ? 1 : 0,
                            bTimerInvoked ? 1 : 0,
                            bObservedAfterCall ? 1 : 0);
                        if (bObservedAfterCall)
                        {
                            State
                                .bAshtonPromotionSkydiving =
                                true;
                            CancelAshtonLeaderPromotion(
                                PlayerController);
                            return;
                        }
                    }
                    else if (
                        bMayReplaceTimer &&
                        (!bTimerQuerySupported ||
                         Now >=
                             SkydiveTimerReplacementAt))
                    {
                        // If cancellation cannot be verified, do not invoke a
                        // second copy of the authored callback.
                        BeginCarminePromotionSkydiveFallback(
                            PlayerController, Pawn);
                        State.bAshtonPromotionSkydiving =
                            true;
                        SDK::DbgLog(
                            "[Ashton1040] applied Carmine "
                            "skydive hard fallback leader=%p "
                            "pawn=%p timerQuery=%d "
                            "timerActive=%d\n",
                            static_cast<void*>(
                                PlayerController),
                            static_cast<void*>(Pawn),
                            bTimerQuerySupported ? 1 : 0,
                            bTimerActive ? 1 : 0);
                        CancelAshtonLeaderPromotion(
                            PlayerController);
                        return;
                    }
                }
                else if (
                    Now >=
                    State
                            .AshtonPromotionSkydiveFallbackAt +
                        0.75)
                {
                    bool bTimerQuerySupported = false;
                    if (IsAshtonPassiveTimerActive(
                            PlayerController,
                            L"SkydiveTimer",
                            bTimerQuerySupported))
                    {
                        ClearAshtonPassiveTimer(
                            PlayerController,
                            L"SkydiveTimer");
                    }
                    BeginCarminePromotionSkydiveFallback(
                        PlayerController, Pawn);
                    State.bAshtonPromotionSkydiving =
                        true;
                    SDK::DbgLog(
                        "[Ashton1040] applied Carmine "
                        "skydive fallback leader=%p "
                        "pawn=%p timerQuery=%d\n",
                        static_cast<void*>(
                            PlayerController),
                        static_cast<void*>(Pawn),
                        bTimerQuerySupported ? 1 : 0);
                    CancelAshtonLeaderPromotion(
                        PlayerController);
                }
            }
            return;
        }

        if (!State.bAshtonPromotionTeleported &&
            Elapsed >= 0.50)
        {
            FVector Location =
                Pawn->K2_GetActorLocation();
            bool bNativeTeleportObserved =
                Location.Z >=
                    State.AshtonPromotionStartZ +
                        1500.0;
            bool bNativeTimerInvoked = false;
            if (!bNativeTeleportObserved)
            {
                bNativeTimerInvoked =
                    InvokeAshtonPassiveTimer(
                        PlayerController,
                        "TeleportTimer");
                Location =
                    Pawn->K2_GetActorLocation();
                bNativeTeleportObserved =
                    Location.Z >=
                        State.AshtonPromotionStartZ +
                            1500.0;
            }
            if (!bNativeTeleportObserved)
            {
                Location.Z += 3000.0;
                Pawn->K2_TeleportTo(
                    Location,
                    Pawn->K2_GetActorRotation());
                if (Pawn->CharacterMovement &&
                    Pawn->CharacterMovement
                        ->GetFunction(
                            "SetMovementMode"))
                {
                    Pawn->CharacterMovement
                        ->SetMovementMode(
                            EMovementMode::MOVE_None,
                            0);
                }
            }
            Pawn->ForceNetUpdate();
            State.bAshtonPromotionTeleported = true;
            SDK::DbgLog(
                "[Ashton1040] completed Ashton lift "
                "leader=%p nativeTimer=%d nativeTeleport=%d z=%.1f\n",
                static_cast<void*>(PlayerController),
                bNativeTimerInvoked ? 1 : 0,
                bNativeTeleportObserved ? 1 : 0,
                Pawn->K2_GetActorLocation().Z);
        }

        if (!State.bAshtonPromotionSkydiving &&
            Elapsed >= 2.0)
        {
            const bool bNativeTimerInvoked =
                InvokeAshtonPassiveTimer(
                    PlayerController,
                    "SkydiveTimer");
            if (!bNativeTimerInvoked)
            {
                ResetAshtonPromotionRuntime(
                    PlayerController, true);
            }
            Pawn->ForceNetUpdate();
            PlayerController->ForceNetUpdate();
            State.bAshtonPromotionSkydiving = true;
            SDK::DbgLog(
                "[Ashton1040] completed Ashton skydive "
                "leader=%p pawn=%p nativeTimer=%d\n",
                static_cast<void*>(PlayerController),
                static_cast<void*>(Pawn),
                bNativeTimerInvoked ? 1 : 0);
            CancelAshtonLeaderPromotion(
                PlayerController);
        }
    }

    AFortPickupAthena* FindActiveAshtonStonePickup(
        UWorld* World,
        int32 StoneType,
        AFortPlayerPawnAthena* PreferredPawn)
    {
        UClass* PickupClass =
            ResolveAshtonRockPickupClass();
        if (!World || !PickupClass ||
            StoneType < 0 || StoneType >= 6)
        {
            return nullptr;
        }

        AFortPickupAthena* FirstMatch = nullptr;
        auto Actors =
            UGameplayStatics::GetAllActorsOfClass(
                World, PickupClass);
        if (IsSaneArray(
                Actors.Num(), Actors.Max(), 64) &&
            IsReadableArrayStorage(
                Actors.Data,
                Actors.Num(),
                sizeof(AActor*)))
        {
            for (auto Actor : Actors)
            {
                auto Pickup =
                    IsLiveObject(Actor) &&
                            Actor->IsA(PickupClass)
                        ? Actor->Cast<
                              AFortPickupAthena>()
                        : nullptr;
                if (!Pickup ||
                    !Pickup->HasAuthority() ||
                    (Pickup
                         ->HasbActorIsBeingDestroyed() &&
                     Pickup
                         ->bActorIsBeingDestroyed) ||
                    (Pickup->HasbPickedUp() &&
                     Pickup->bPickedUp) ||
                    GetAshtonStoneType(
                        Pickup
                            ->PrimaryPickupItemEntry
                            .ItemDefinition) !=
                        StoneType)
                {
                    continue;
                }

                if (!FirstMatch)
                    FirstMatch = Pickup;
                if (PreferredPawn &&
                    Pickup->PickupLocationData
                            .PickupTarget ==
                        PreferredPawn)
                {
                    FirstMatch = Pickup;
                    break;
                }
            }
        }
        Actors.Free();
        return FirstMatch;
    }

    void RetireAshtonStonePickup(
        AFortPickupAthena* Pickup,
        AFortPlayerPawnAthena* Collector)
    {
        if (!IsLiveObject(Pickup) ||
            !Pickup->HasAuthority() ||
            (Pickup->HasbActorIsBeingDestroyed() &&
             Pickup->bActorIsBeingDestroyed))
        {
            return;
        }

        if (Collector)
        {
            Pickup->PickupLocationData
                .PickupTarget = Collector;
        }
        if (!Pickup->HasbPickedUp() ||
            !Pickup->bPickedUp)
        {
            Pickup->bPickedUp = true;
            Pickup->FlushNetDormancy();
            Pickup->OnRep_bPickedUp();
            Pickup->ForceNetUpdate();
        }
        Pickup->SetLifeSpan(0.01f);
    }

    UFortWorldItem* FindAshtonInventoryItemInstance(
        AFortInventory* Inventory,
        const FGuid& ItemGuid)
    {
        if (!IsLiveObject(Inventory))
            return nullptr;
        auto Match =
            Inventory->Inventory.ItemInstances.Search(
                [&](UFortWorldItem* Item)
                {
                    return IsLiveObject(Item) &&
                        Item->ItemEntry.ItemGuid.A ==
                            ItemGuid.A &&
                        Item->ItemEntry.ItemGuid.B ==
                            ItemGuid.B &&
                        Item->ItemEntry.ItemGuid.C ==
                            ItemGuid.C &&
                        Item->ItemEntry.ItemGuid.D ==
                            ItemGuid.D;
                });
        return Match ? *Match : nullptr;
    }

    bool EnsureAshtonLeaderItem(
        AFortAthenaMutator_Ashton* Ashton,
        AFortPlayerControllerAthena* PlayerController,
        const char* Reason)
    {
        if (!IsLiveObject(Ashton) ||
            !IsValidAshtonLeaderController(
                PlayerController) ||
            GAuthoredNativeLTMPhaseState
                .bAshtonLeaderVacant ||
            !Ashton->HasVillainLeaderPC() ||
            Ashton->VillainLeaderPC !=
                PlayerController ||
            !Ashton->HasVillainLeaderItemDef() ||
            !IsLiveObject(
                Ashton->VillainLeaderItemDef))
        {
            return false;
        }

        auto Inventory =
            PlayerController->WorldInventory;
        auto LeaderDefinition =
            Ashton->VillainLeaderItemDef;
        auto Existing =
            Inventory->Inventory.ReplicatedEntries
                .Search(
                    [&](FFortItemEntry& Entry)
                    {
                        return Entry.ItemDefinition ==
                            LeaderDefinition;
                    },
                    FFortItemEntry::Size());
        auto& InitializedItems =
            GAuthoredNativeLTMPhaseState
                .AshtonInitializedLeaderItems;
        auto Initialized =
            InitializedItems.find(PlayerController);
        const bool bExistingAlreadyInitialized =
            Existing &&
            Initialized != InitializedItems.end() &&
            Initialized->second == Existing->ItemGuid;
        if (!bExistingAlreadyInitialized &&
            LeaderDefinition->Name.ToWString() ==
                L"AGID_CarminePack")
        {
            // InstancedPerActor abilities can survive a remove/regrant.
            // Rotate the one-shot before the new gadget activation so this
            // promotion receives one authored AnimBP-ready sequence while a
            // late callback from the same activation remains suppressible.
            PrepareCarminePassiveReadyActivation(
                PlayerController);
        }

        UFortWorldItem* LeaderItem = nullptr;
        FGuid LeaderGuid{};
        bool bGranted = false;
        bool bGrantFocusHandled = false;
        bool bGrantInitializationDispatched =
            false;
        if (Existing)
        {
            LeaderGuid = Existing->ItemGuid;
            LeaderItem =
                FindAshtonInventoryItemInstance(
                    Inventory, LeaderGuid);
            if (!LeaderItem)
            {
                Inventory->HandleInventoryLocalUpdate();
                LeaderItem =
                    FindAshtonInventoryItemInstance(
                        Inventory, LeaderGuid);
            }
        }
        else
        {
            LeaderItem = Inventory->GiveItem(
                LeaderDefinition,
                1,
                0,
                0,
                true,
                true,
                0,
                {},
                true,
                {},
                &bGrantFocusHandled,
                &bGrantInitializationDispatched);
            if (!LeaderItem)
                return false;
            LeaderGuid =
                LeaderItem->ItemEntry.ItemGuid;
            bGranted = true;
        }

        const bool bAlreadyInitialized =
            Initialized != InitializedItems.end() &&
            Initialized->second == LeaderGuid;
        bool bInitialized =
            bAlreadyInitialized ||
            (bGranted &&
             bGrantInitializationDispatched);
        bool bActivationAttempted = false;
        if (!bAlreadyInitialized &&
            !bInitialized &&
            LeaderItem)
        {
            bInitialized =
                Inventory
                    ->InitializeGadgetItemWithFallback(
                        LeaderItem, true);
            bActivationAttempted = true;
        }
        if (!bAlreadyInitialized &&
            !bInitialized)
        {
            // No item-added callback was dispatched. Keep this GUID retryable
            // without focusing the client on the base gadget's missing local
            // D_AshtonPack soft reference.
            SDK::DbgLog(
                "[Ashton1040] deferred Thanos gadget "
                "activation collector=%p guid=%08X-%08X-%08X-%08X "
                "reason=%s\n",
                static_cast<void*>(PlayerController),
                static_cast<unsigned>(LeaderGuid.A),
                static_cast<unsigned>(LeaderGuid.B),
                static_cast<unsigned>(LeaderGuid.C),
                static_cast<unsigned>(LeaderGuid.D),
                Reason ? Reason : "unknown");
            return false;
        }
        if (!bAlreadyInitialized)
        {
            // This GUID received exactly one stock notification. Never reapply
            // its ability set from the watchdog: doing so duplicates
            // persistent cue tags until the purple beam stops replicating.
            InitializedItems[PlayerController] =
                LeaderGuid;
            // Carmine's native passive owns the complete Endgame transition.
            // Queue an observational watchdog for its 1.0-second 35k lift and
            // 3.43-second skydive; it waits for native outcomes before ever
            // invoking the same authored timers. The incomplete legacy base
            // gadget retains its older compatibility branch.
            const auto LeaderName =
                LeaderDefinition->Name.ToWString();
            if (LeaderName == L"AGID_CarminePack" ||
                LeaderName == L"AGID_AshtonPack")
            {
                QueueAshtonLeaderPromotion(
                    PlayerController, LeaderGuid);
            }
        }

        auto BackingDefinition =
            GetAshtonLeaderBackingDefinition(
                Ashton);
        auto BackingEntry =
            BackingDefinition
                ? Inventory->Inventory
                      .ReplicatedEntries.Search(
                          [&](FFortItemEntry& Entry)
                          {
                              return Entry
                                         .ItemDefinition ==
                                  BackingDefinition;
                          },
                          FFortItemEntry::Size())
                : nullptr;
        auto& InitializedBackings =
            GAuthoredNativeLTMPhaseState
                .AshtonInitializedLeaderBackingItems;
        auto InitializedBacking =
            InitializedBackings.find(
                PlayerController);
        bool bBackingReady =
            BackingEntry &&
            InitializedBacking !=
                InitializedBackings.end() &&
            InitializedBacking->second.A ==
                BackingEntry->ItemGuid.A &&
            InitializedBacking->second.B ==
                BackingEntry->ItemGuid.B &&
            InitializedBacking->second.C ==
                BackingEntry->ItemGuid.C &&
            InitializedBacking->second.D ==
                BackingEntry->ItemGuid.D;
        if (!bBackingReady)
        {
            FGuid BackingGuid{};
            if (bGranted &&
                bGrantFocusHandled &&
                BackingEntry)
            {
                BackingGuid =
                    BackingEntry->ItemGuid;
                bBackingReady = true;
            }
            else
            {
                bBackingReady =
                    Inventory
                        ->EnsureExact1040AshtonBackingAndFocus(
                            &BackingGuid);
            }
            if (bBackingReady)
            {
                InitializedBackings[
                    PlayerController] =
                    BackingGuid;
            }
        }

        Inventory->ForceNetUpdate();
        PlayerController->ForceNetUpdate();
        SDK::DbgLog(
            "[Ashton1040] ensured Thanos gadget "
            "collector=%p definition=%s "
            "guid=%08X-%08X-%08X-%08X "
            "granted=%d grantInit=%d grantFocus=%d attempted=%d "
            "initialized=%d backingReady=%d oneShot=%d reason=%s\n",
            static_cast<void*>(PlayerController),
            LeaderDefinition->Name
                .ToString().c_str(),
            static_cast<unsigned>(LeaderGuid.A),
            static_cast<unsigned>(LeaderGuid.B),
            static_cast<unsigned>(LeaderGuid.C),
            static_cast<unsigned>(LeaderGuid.D),
            bGranted ? 1 : 0,
            bGrantInitializationDispatched ? 1 : 0,
            bGrantFocusHandled ? 1 : 0,
            bActivationAttempted ? 1 : 0,
            bInitialized ? 1 : 0,
            bBackingReady ? 1 : 0,
            bAlreadyInitialized ? 0 : 1,
            Reason ? Reason : "unknown");
        return LeaderItem != nullptr &&
            bBackingReady;
    }

    void DemoteIncorrectAshtonLeader(
        AFortAthenaMutator_Ashton* Ashton,
        AFortPlayerControllerAthena* PlayerController,
        const char* Reason)
    {
        if (!IsLiveObject(PlayerController))
        {
            return;
        }

        RemoveAllAshtonLeaderArtifacts(
            Ashton,
            PlayerController);
        if (!GAuthoredNativeLTMPhaseState
                 .AshtonEliminatedLeaders.contains(
                     PlayerController))
        {
            ReconcileAshtonVillainPlayer(
                PlayerController,
                Reason ? Reason :
                    "stone-leader-correction");
        }
    }

    void FinalizeAshtonStoneLeader(
        AFortAthenaMutator_Ashton* Ashton,
        AFortPlayerControllerAthena* Collector,
        AFortPlayerControllerAthena* LeaderBefore,
        const char* Reason)
    {
        if (!IsLiveObject(Ashton) ||
            !IsValidAshtonLeaderController(
                Collector))
        {
            return;
        }

        RemoveAshtonInventoryRows(
            Collector, nullptr, true);

        auto& State =
            GAuthoredNativeLTMPhaseState;
        const bool bKeepExistingLeader =
            !State.bAshtonLeaderVacant &&
            IsValidAshtonLeaderController(
                LeaderBefore);
        auto DesiredLeader =
            bKeepExistingLeader
                ? LeaderBefore
                : Collector;
        auto NativeLeader =
            Ashton->HasVillainLeaderPC()
                ? Ashton->VillainLeaderPC
                : nullptr;

        State.AshtonAuthorizedAllStoneFallback =
            nullptr;
        State.bAshtonLeaderVacant = false;
        if (Ashton->HasVillainLeaderPC())
        {
            // Publish first so reconciliation cannot mistake the native
            // candidate for the still-authoritative leader.
            Ashton->VillainLeaderPC =
                DesiredLeader;
            Ashton->ForceNetUpdate();
            if (NativeLeader &&
                NativeLeader != DesiredLeader)
            {
                DemoteIncorrectAshtonLeader(
                    Ashton,
                    NativeLeader,
                    "stone-native-leader-correction");
            }
            // A proven stone capture is the only path that fills a vacant
            // leader slot. Later stones keep the one still-living leader.
        }

        UWorld* World = UWorld::GetWorld();
        auto GameState =
            World && World->GameState
                ? World->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        if (IsLiveObject(GameState))
        {
            // Sweep every villain, not just the native candidate, so there can
            // never be two surviving Ashton gadgets after a transfer race.
            ReconcileAshtonVillainPlayers(
                GameState,
                Reason ? Reason : "stone-leader");
        }
        else
        {
            if (DesiredLeader != Collector)
            {
                ReconcileAshtonVillainPlayer(
                    Collector,
                    "later-stone-collector");
            }
            ReconcileAshtonVillainPlayer(
                DesiredLeader,
                Reason ? Reason : "stone-leader");
        }
    }

    bool MarkAshtonStoneCapturedFallback(
        AFortAthenaMutator_Ashton* Ashton,
        int32 StoneType)
    {
        auto Stone =
            FindAshtonStoneState(
                Ashton, StoneType);
        if (!Stone ||
            Stone->StoneState ==
                AshtonStoneStateCaptured)
        {
            return Stone &&
                Stone->StoneState ==
                    AshtonStoneStateCaptured;
        }

        Stone->SetStoneState(
            uint8(AshtonStoneStateCaptured));
        if (Stone->HasbHasEverSpawned())
            Stone->bHasEverSpawned = true;
        if (Ashton->GetFunction(
                "OnRep_StoneList"))
        {
            Ashton->OnRep_StoneList();
        }
        Ashton->ForceNetUpdate();
        return true;
    }

    void RetireDuplicateAshtonPickup(
        AFortPickupAthena* Pickup)
    {
        if (!IsLiveObject(Pickup) ||
            !Pickup->HasAuthority() ||
            (Pickup->HasbActorIsBeingDestroyed() &&
             Pickup->bActorIsBeingDestroyed))
        {
            return;
        }

        // A duplicate's destruction is cleanup, not a capture. Remove the
        // objective definition before destruction so FortGameModePickup's
        // lifecycle cannot rebroadcast it as a collected stone.
        Pickup->PrimaryPickupItemEntry
            .ItemDefinition = nullptr;
        Pickup->OnRep_PrimaryPickupItemEntry();
        Pickup->bPickedUp = true;
        Pickup->OnRep_bPickedUp();
        Pickup->ForceNetUpdate();
        Pickup->K2_DestroyActor();
    }

    void RetireDuplicateAshtonSupply(
        AActor* Supply)
    {
        if (!IsLiveObject(Supply) ||
            !Supply->HasAuthority() ||
            (Supply->HasbActorIsBeingDestroyed() &&
             Supply->bActorIsBeingDestroyed))
        {
            return;
        }

        // AthenaSupplyDrop_RockBase owns this reflected definition and uses it
        // when landing opens the carrier. Clear it before retiring an extra so
        // its teardown cannot materialize a replacement objective.
        auto ItemDefinitionProperty =
            Supply->GetProperty("ItemDefToSpawn");
        if (ItemDefinitionProperty)
        {
            const uint32 Offset =
                GetFromOffset<uint32>(
                    ItemDefinitionProperty,
                    Offsets::Offset_Internal);
            const uint32 ElementSize =
                GetFromOffset<uint32>(
                    ItemDefinitionProperty,
                    Offsets::ElementSize);
            if (Offset != uint32(-1) &&
                ElementSize == sizeof(
                    UFortWorldItemDefinition*))
            {
                GetFromOffset<
                    UFortWorldItemDefinition*>(
                    Supply, Offset) = nullptr;
            }
        }
        Supply->K2_DestroyActor();
    }

    void ReconcileAshtonStonePickups(
        AFortGameStateAthena* GameState,
        AFortAthenaMutator_Ashton* Ashton,
        double Now)
    {
        UWorld* World = UWorld::GetWorld();
        if (!IsLiveObject(GameState) ||
            !IsLiveObject(Ashton) ||
            !HasInitializedAshtonStoneRuntime(Ashton) ||
            !World ||
            World->GameState != GameState ||
            GNativeLTMCompatibilityState.World != World ||
            !IsNativeAshtonDescriptor(
                GNativeLTMCompatibilityState.Descriptor) ||
            !GameState->HasAuthority() ||
            !Ashton->HasAuthority())
        {
            return;
        }

        static constexpr const wchar_t* Suffixes[] = {
            L"P", L"B", L"R", L"O", L"G", L"Y"
        };
        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        UClass* PickupClass =
            ResolveAshtonRockPickupClass();
        if (!PickupClass)
            return;

        std::vector<AFortPickupAthena*>
            PickupsByStone[6];
        auto AllPickups =
            UGameplayStatics::GetAllActorsOfClass(
                World, PickupClass);
        if (IsSaneArray(
                AllPickups.Num(),
                AllPickups.Max(),
                128) &&
            IsReadableArrayStorage(
                AllPickups.Data,
                AllPickups.Num(),
                sizeof(AActor*)))
        {
            for (auto Actor : AllPickups)
            {
                auto Pickup =
                    IsLiveObject(Actor) &&
                            Actor->IsA(PickupClass)
                        ? Actor->Cast<
                              AFortPickupAthena>()
                        : nullptr;
                if (!Pickup ||
                    !Pickup->HasAuthority() ||
                    (Pickup
                         ->HasbActorIsBeingDestroyed() &&
                     Pickup
                         ->bActorIsBeingDestroyed))
                {
                    continue;
                }

                const int32 StoneType =
                    GetAshtonStoneType(
                        Pickup
                            ->PrimaryPickupItemEntry
                            .ItemDefinition);
                if (StoneType >= 0 &&
                    StoneType < 6)
                {
                    PickupsByStone[StoneType]
                        .push_back(Pickup);
                }
            }
        }
        AllPickups.Free();

        for (int32 StoneIndex = 0;
             StoneIndex < 6;
             ++StoneIndex)
        {
            auto StateRow =
                FindAshtonStoneState(
                    Ashton, StoneIndex);
            if (StateRow &&
                PhaseState
                    .bAshtonStoneCaptureLocked[
                        StoneIndex] &&
                StateRow->StoneState !=
                    AshtonStoneStateCaptured)
            {
                const uint8 PreviousState =
                    StateRow->StoneState;
                StateRow->SetStoneState(
                    uint8(
                        AshtonStoneStateCaptured));
                if (Ashton->GetFunction(
                        "OnRep_StoneList"))
                {
                    Ashton->OnRep_StoneList();
                }
                Ashton->ForceNetUpdate();
                SDK::DbgLog(
                    "[Ashton1040] restored captured stone "
                    "lock type=%d nativeState=%u\n",
                    StoneIndex,
                    static_cast<unsigned>(
                        PreviousState));
            }

            const bool bSpawnedGeneration =
                StateRow &&
                StateRow->StoneState ==
                    AshtonStoneStateSpawned;
            if (!bSpawnedGeneration)
            {
                // The next transition into Spawned is a genuinely new
                // authored generation. Actor disappearance alone must never
                // re-arm compatibility restoration for the current one.
                PhaseState
                    .bAshtonStoneRecoveryGenerationInitialized[
                        StoneIndex] = false;
                PhaseState
                    .AshtonStoneCompatibilityRestoreAttempts[
                        StoneIndex] = 0;
            }
            else
            {
                const float RecoveryGeneration =
                    FAshtonStoneState::HasSpawnTime() &&
                            std::isfinite(
                                StateRow->SpawnTime)
                        ? StateRow->SpawnTime
                        : 0.0f;
                const bool bNewGeneration =
                    !PhaseState
                         .bAshtonStoneRecoveryGenerationInitialized[
                             StoneIndex] ||
                    std::abs(
                        PhaseState
                                .AshtonStoneRecoveryGeneration[
                                    StoneIndex] -
                            RecoveryGeneration) >
                        0.01f;
                if (bNewGeneration)
                {
                    PhaseState
                        .bAshtonStoneRecoveryGenerationInitialized[
                            StoneIndex] = true;
                    PhaseState
                        .AshtonStoneRecoveryGeneration[
                            StoneIndex] =
                        RecoveryGeneration;
                    PhaseState
                        .AshtonStoneCompatibilityRestoreAttempts[
                            StoneIndex] = 0;
                }
            }

            const bool bHasValidSpawnDeadline =
                StateRow &&
                FAshtonStoneState::HasSpawnTime() &&
                std::isfinite(StateRow->SpawnTime) &&
                StateRow->SpawnTime > 0.0f;
            if (StateRow &&
                StateRow->StoneState ==
                    AshtonStoneStateSpawned &&
                bHasValidSpawnDeadline &&
                Now + 0.05 <
                    static_cast<double>(
                        StateRow->SpawnTime))
            {
                // SpawnTime is the mutator's absolute server-world deadline
                // used by the HUD. A Spawned enum value means the native
                // meteor is scheduled, not that its carrier must already
                // exist. Honor a future deadline even if native code has
                // already latched bHasEverSpawned while scheduling it.
                // Starting compatibility recovery before this deadline
                // created a ground gem and then let the authored meteor create
                // another one.
                PhaseState
                    .AshtonStoneMissingSince[
                        StoneIndex] = 0.0;
                PhaseState
                    .AshtonStoneGroundedSupplySince[
                        StoneIndex] = 0.0;
                PhaseState
                    .bAshtonSawStoneSupply[
                        StoneIndex] = false;
                if (!PhaseState
                         .bLoggedAshtonStoneSpawnDeadline[
                             StoneIndex])
                {
                    PhaseState
                        .bLoggedAshtonStoneSpawnDeadline[
                            StoneIndex] = true;
                    SDK::DbgLog(
                        "[Ashton1040] waiting for authored "
                        "stone deadline type=%d now=%.2f "
                        "spawnTime=%.2f\n",
                        StoneIndex,
                        Now,
                        static_cast<double>(
                            StateRow->SpawnTime));
                }
                continue;
            }

            std::wstring SupplyPath =
                L"/Game/Athena/Playlists/Ashton/Rocks/"
                L"AthenaSupplyDrop_Rock_";
            SupplyPath += Suffixes[StoneIndex];
            SupplyPath += L".AthenaSupplyDrop_Rock_";
            SupplyPath += Suffixes[StoneIndex];
            SupplyPath += L"_C";
            auto SupplyClass =
                const_cast<UClass*>(
                    FindObject<UClass>(
                        SupplyPath.c_str()));
            std::vector<AActor*> SuppliesForStone;
            if (SupplyClass)
            {
                auto Supplies =
                    UGameplayStatics::GetAllActorsOfClass(
                        World, SupplyClass);
                if (IsSaneArray(
                        Supplies.Num(),
                        Supplies.Max(),
                        64) &&
                    IsReadableArrayStorage(
                        Supplies.Data,
                        Supplies.Num(),
                        sizeof(AActor*)))
                {
                    for (auto Actor : Supplies)
                    {
                        if (IsLiveObject(Actor) &&
                            Actor->Class ==
                                SupplyClass &&
                            Actor->HasAuthority() &&
                            (!Actor
                                  ->HasbActorIsBeingDestroyed() ||
                             !Actor
                                  ->bActorIsBeingDestroyed))
                        {
                            SuppliesForStone
                                .push_back(Actor);
                        }
                    }
                }
                Supplies.Free();
            }

            if (!StateRow ||
                StateRow->StoneState !=
                    AshtonStoneStateSpawned)
            {
                const int32 RetiredPickups =
                    static_cast<int32>(
                        PickupsByStone[StoneIndex]
                            .size());
                const int32 RetiredSupplies =
                    static_cast<int32>(
                        SuppliesForStone.size());
                for (auto Pickup :
                     PickupsByStone[StoneIndex])
                {
                    RetireDuplicateAshtonPickup(
                        Pickup);
                }
                for (auto Supply : SuppliesForStone)
                {
                    RetireDuplicateAshtonSupply(
                        Supply);
                }

                PhaseState
                    .AshtonStoneMissingSince[
                        StoneIndex] = 0.0;
                PhaseState
                    .AshtonStoneGroundedSupplySince[
                        StoneIndex] = 0.0;
                PhaseState
                    .bAshtonSawStoneSupply[
                        StoneIndex] = false;
                if (RetiredPickups > 0 ||
                    RetiredSupplies > 0)
                {
                    SDK::DbgLog(
                        "[Ashton1040] retired inactive stone "
                        "actors type=%d state=%u pickups=%d "
                        "supplies=%d\n",
                        StoneIndex,
                        StateRow
                            ? static_cast<unsigned>(
                                  StateRow->StoneState)
                            : 255u,
                        RetiredPickups,
                        RetiredSupplies);
                }
                // Captured and not-yet-spawned rows can never enter fallback
                // restoration from this pass.
                continue;
            }

            std::vector<AFortPickupAthena*>
                ActivePickups;
            ActivePickups.reserve(
                PickupsByStone[StoneIndex].size());
            for (auto Pickup :
                 PickupsByStone[StoneIndex])
            {
                if (Pickup->HasbPickedUp() &&
                    Pickup->bPickedUp)
                {
                    RetireDuplicateAshtonPickup(
                        Pickup);
                    continue;
                }
                ActivePickups.push_back(Pickup);
            }

            if (!ActivePickups.empty())
            {
                // A ground objective supersedes every remaining carrier for
                // the same color. Keep one exact common pickup and retire all
                // other pickup/supply actors before the next watchdog pass.
                for (size_t Index = 1;
                     Index < ActivePickups.size();
                     ++Index)
                {
                    RetireDuplicateAshtonPickup(
                        ActivePickups[Index]);
                }
                for (auto Supply : SuppliesForStone)
                {
                    RetireDuplicateAshtonSupply(
                        Supply);
                }

                PhaseState
                    .AshtonStoneMissingSince[
                        StoneIndex] = 0.0;
                PhaseState
                    .AshtonStoneGroundedSupplySince[
                        StoneIndex] = 0.0;
                if (ActivePickups.size() > 1 ||
                    !SuppliesForStone.empty())
                {
                    SDK::DbgLog(
                        "[Ashton1040] deduplicated spawned "
                        "stone type=%d keptPickup=%p "
                        "retiredPickups=%llu "
                        "retiredSupplies=%llu\n",
                        StoneIndex,
                        static_cast<void*>(
                            ActivePickups.front()),
                        static_cast<
                            unsigned long long>(
                            ActivePickups.size() - 1),
                        static_cast<
                            unsigned long long>(
                            SuppliesForStone.size()));
                }
                continue;
            }

            AActor* KeptSupply =
                SuppliesForStone.empty()
                    ? nullptr
                    : SuppliesForStone.front();
            for (size_t Index = 1;
                 Index < SuppliesForStone.size();
                 ++Index)
            {
                RetireDuplicateAshtonSupply(
                    SuppliesForStone[Index]);
            }
            const bool bHasSupply =
                IsLiveObject(KeptSupply);
            if (bHasSupply)
            {
                const FVector Location =
                    KeptSupply->K2_GetActorLocation();
                if (std::isfinite(Location.X) &&
                    std::isfinite(Location.Y) &&
                    std::isfinite(Location.Z))
                {
                    PhaseState
                        .AshtonLastStoneSupplyLocation[
                            StoneIndex] =
                        FVector(
                            Location.X,
                            Location.Y,
                            Location.Z);
                    PhaseState
                        .bAshtonSawStoneSupply[
                            StoneIndex] = true;
                }
                if (SuppliesForStone.size() > 1)
                {
                    SDK::DbgLog(
                        "[Ashton1040] deduplicated stone "
                        "carrier type=%d keptSupply=%p "
                        "retired=%llu\n",
                        StoneIndex,
                        static_cast<void*>(KeptSupply),
                        static_cast<
                            unsigned long long>(
                            SuppliesForStone.size() - 1));
                }
            }

            double& MissingSince =
                PhaseState
                    .AshtonStoneMissingSince[StoneIndex];
            if (MissingSince <= 0.0)
                MissingSince = Now;

            if (bHasSupply)
            {
                const FVector SupplyLocation =
                    PhaseState
                        .AshtonLastStoneSupplyLocation[
                            StoneIndex];
                const FVector SupplyGroundLocation =
                    UFortKismetLibrary::
                        FindGroundLocationAt(
                            World,
                            nullptr,
                            SupplyLocation,
                            15000.0f,
                            -15000.0f,
                            FName());
                const bool bSupplyNearGround =
                    std::isfinite(
                        SupplyGroundLocation.Z) &&
                    std::abs(
                        SupplyLocation.Z -
                        SupplyGroundLocation.Z) <=
                        300.0f;
                double& GroundedSince =
                    PhaseState
                        .AshtonStoneGroundedSupplySince[
                            StoneIndex];
                if (bSupplyNearGround)
                {
                    if (GroundedSince <= 0.0)
                    {
                        GroundedSince = Now;
                        continue;
                    }
                    // Let the authored auto-open callback win after landing;
                    // recover only if it still did not create its gem.
                    if (Now < GroundedSince + 2.0)
                        continue;
                }
                else
                {
                    GroundedSince = 0.0;
                    // A meteor can legitimately spend several seconds in
                    // flight. A supply actor that never lands must not suppress
                    // recovery forever.
                    if (Now < MissingSince + 30.0)
                        continue;
                }
            }
            else
            {
                PhaseState
                    .AshtonStoneGroundedSupplySince[
                        StoneIndex] = 0.0;
            }

            if (!PhaseState
                     .bAshtonSawStoneSupply[StoneIndex])
            {
                auto GameMode =
                    IsLiveObject(World->AuthorityGameMode)
                        ? World->AuthorityGameMode
                              ->Cast<AFortGameMode>()
                        : nullptr;
                if (!GameMode ||
                    !GameMode->HasSafeZoneLocations())
                {
                    continue;
                }
                const auto& Locations =
                    GameMode->SafeZoneLocations;
                if (!IsSaneArray(
                        Locations.Num(),
                        Locations.Max(),
                        64) ||
                    Locations.Num() <= 0 ||
                    !IsReadableArrayStorage(
                        Locations.Data,
                        Locations.Num(),
                        FVector::Size()))
                {
                    continue;
                }

                const FVector& Fallback =
                    Locations.Get(
                        StoneIndex % Locations.Num(),
                        FVector::Size());
                if (!std::isfinite(Fallback.X) ||
                    !std::isfinite(Fallback.Y) ||
                    !std::isfinite(Fallback.Z))
                {
                    continue;
                }
                PhaseState
                    .AshtonLastStoneSupplyLocation[
                        StoneIndex] =
                    FVector(
                        Fallback.X,
                        Fallback.Y,
                        Fallback.Z);
            }

            // Give the authored supply actor time to materialize after the
            // state row changes. Once a supply actor was observed and then
            // vanished, the shorter grace repairs its missing landing pickup.
            const double RecoveryGrace =
                bHasSupply
                    ? 0.0
                    : PhaseState
                        .bAshtonSawStoneSupply[StoneIndex]
                        ? 1.50
                        : 8.00;
            if (Now < MissingSince + RecoveryGrace)
                continue;

            if (PhaseState
                    .AshtonStoneCompatibilityRestoreAttempts[
                        StoneIndex] >= 1)
            {
                // One compatibility pickup is the terminal recovery for this
                // authored SpawnTime generation. If some unrelated callback
                // destroys it, do not create an endless stream of duplicates.
                continue;
            }

            std::wstring ItemPath =
                L"/Game/Athena/Items/LTM/"
                L"AshtonRockItemDef_";
            ItemPath += Suffixes[StoneIndex];
            ItemPath += L".AshtonRockItemDef_";
            ItemPath += Suffixes[StoneIndex];
            auto ItemDefinition =
                FindObject<UFortWorldItemDefinition>(
                    ItemPath.c_str());
            if (!ItemDefinition)
                continue;

            // A grounded/stalled carrier is no longer allowed to coexist with
            // the restored common pickup. Suppress its authored drop first,
            // then create exactly one replacement objective.
            if (bHasSupply)
            {
                RetireDuplicateAshtonSupply(
                    KeptSupply);
                KeptSupply = nullptr;
            }

            const auto& LastSupplyLocation =
                PhaseState
                    .AshtonLastStoneSupplyLocation[
                        StoneIndex];
            FVector SpawnLocation(
                LastSupplyLocation.X,
                LastSupplyLocation.Y,
                LastSupplyLocation.Z);
            const FVector GroundLocation =
                UFortKismetLibrary::FindGroundLocationAt(
                    World,
                    nullptr,
                    SpawnLocation,
                    15000.0f,
                    -15000.0f,
                    FName());
            if (std::isfinite(GroundLocation.X) &&
                std::isfinite(GroundLocation.Y) &&
                std::isfinite(GroundLocation.Z))
            {
                SpawnLocation = FVector(
                    GroundLocation.X,
                    GroundLocation.Y,
                    GroundLocation.Z);
            }
            SpawnLocation.Z += 30.0f;

            auto Pickup = AFortInventory::SpawnPickup(
                SpawnLocation,
                ItemDefinition,
                1,
                -1,
                EFortPickupSourceTypeFlag::GetOther(),
                EFortPickupSpawnSource::GetSupplyDrop(),
                nullptr,
                false,
                false,
                PickupClass);
            if (!Pickup)
                continue;

            // FortGameModePickup inherits a finite default lifespan in this
            // build. A restored objective must remain until capture, just like
            // the authored rock pickup, or the watchdog would see it disappear
            // and repeatedly recreate the same stone.
            Pickup->SetLifeSpan(0.0f);
            Pickup->ForceNetUpdate();
            ++PhaseState
                  .AshtonStoneCompatibilityRestoreAttempts[
                      StoneIndex];
            MissingSince = 0.0;
            PhaseState
                .AshtonStoneGroundedSupplySince[
                    StoneIndex] = 0.0;
            SDK::DbgLog(
                "[Ashton1040] restored missing stone "
                "pickup type=%d item=%s class=%s\n",
                StoneIndex,
                ItemDefinition->Name.ToString().c_str(),
                PickupClass->Name.ToString().c_str());
        }
    }

    bool HasReadyDiscoSafeZoneGeometry()
    {
        UWorld* World = UWorld::GetWorld();
        auto GameMode = World && IsLiveObject(World->AuthorityGameMode)
            ? World->AuthorityGameMode->Cast<AFortGameMode>()
            : nullptr;
        if (!GameMode ||
            !GameMode->HasSafeZoneLocations() ||
            (GameMode->HasbSafeZoneLocationsInitialized() &&
             !GameMode->bSafeZoneLocationsInitialized))
        {
            return false;
        }

        const auto& Locations = GameMode->SafeZoneLocations;
        if (!IsSaneArray(
                Locations.Num(),
                Locations.Max(),
                64) ||
            // The exact 10.40 Disco spawn table references safe-zone
            // locations 0 through 4.
            Locations.Num() < 5 ||
            !IsReadableArrayStorage(
                Locations.Data,
                Locations.Num(),
                FVector::Size()))
        {
            return false;
        }

        for (int32 Index = 0;
             Index < Locations.Num();
             ++Index)
        {
            const FVector& Location =
                Locations.Get(Index, FVector::Size());
            if (!std::isfinite(Location.X) ||
                !std::isfinite(Location.Y) ||
                !std::isfinite(Location.Z))
            {
                return false;
            }
        }
        return true;
    }

    bool HasReadyAshtonSafeZoneGeometry()
    {
        UWorld* World = UWorld::GetWorld();
        auto GameMode =
            World && IsLiveObject(World->AuthorityGameMode)
                ? World->AuthorityGameMode
                      ->Cast<AFortGameMode>()
                : nullptr;
        if (!GameMode ||
            !GameMode->HasSafeZoneLocations() ||
            (GameMode->HasbSafeZoneLocationsInitialized() &&
             !GameMode->bSafeZoneLocationsInitialized))
        {
            return false;
        }

        const auto& Locations =
            GameMode->SafeZoneLocations;
        if (!IsSaneArray(
                Locations.Num(), Locations.Max(), 64) ||
            Locations.Num() < 1 ||
            !IsReadableArrayStorage(
                Locations.Data,
                Locations.Num(),
                FVector::Size()))
        {
            return false;
        }

        for (int32 Index = 0;
             Index < Locations.Num();
             ++Index)
        {
            const FVector& Location =
                Locations.Get(Index, FVector::Size());
            if (!std::isfinite(Location.X) ||
                !std::isfinite(Location.Y) ||
                !std::isfinite(Location.Z))
            {
                return false;
            }
        }
        return true;
    }

    bool AreDiscoPointsCoincident(
        AActor* Left,
        AActor* Right)
    {
        if (!IsLiveObject(Left) || !IsLiveObject(Right))
            return false;

        const FVector LeftLocation =
            Left->K2_GetActorLocation();
        const FVector RightLocation =
            Right->K2_GetActorLocation();
        if (!std::isfinite(LeftLocation.X) ||
            !std::isfinite(LeftLocation.Y) ||
            !std::isfinite(LeftLocation.Z) ||
            !std::isfinite(RightLocation.X) ||
            !std::isfinite(RightLocation.Y) ||
            !std::isfinite(RightLocation.Z))
        {
            return false;
        }

        const double DeltaX =
            LeftLocation.X - RightLocation.X;
        const double DeltaY =
            LeftLocation.Y - RightLocation.Y;
        const double DeltaZ =
            LeftLocation.Z - RightLocation.Z;
        const double CoincidentDistanceSquared =
            DiscoCoincidentPointDistance *
            DiscoCoincidentPointDistance;
        return DeltaX * DeltaX +
                DeltaY * DeltaY +
                DeltaZ * DeltaZ <=
            CoincidentDistanceSquared;
    }

    void PrepareDiscoPointReplication(AActor* Point)
    {
        if (!IsLiveObject(Point))
            return;

        if (Point->HasbReplicates() &&
            !Point->bReplicates)
        {
            if (auto SetReplicatesFunction =
                    Point->GetFunction("SetReplicates"))
            {
                Point->Call<void>(
                    SetReplicatesFunction, true);
            }
            else
            {
                Point->bReplicates = true;
            }
        }
        if (Point->HasbAlwaysRelevant())
            Point->bAlwaysRelevant = true;
        if (Point->HasbOnlyRelevantToOwner())
            Point->bOnlyRelevantToOwner = false;
        if (Point->HasbNetUseOwnerRelevancy())
            Point->bNetUseOwnerRelevancy = false;
        if (Point->HasNetCullDistanceSquared())
        {
            Point->NetCullDistanceSquared =
                (std::numeric_limits<float>::max)();
        }
        if (Point->HasNetUpdateFrequency() &&
            Point->NetUpdateFrequency < 10.0f)
        {
            Point->NetUpdateFrequency = 10.0f;
        }

        Point->SetNetDormancy(ENetDormancy::DORM_Never);
        Point->FlushNetDormancy();
        Point->ForceNetUpdate();
    }

    void ResetMalformedDiscoPointRow(
        FControlPointInstanceData& Row)
    {
        // Only repaired duplicate/co-located rows pass through here. Preserve
        // their authored spawn index, schedule and enabled state while
        // discarding ownership/accrual cached from the wrong actor.
        Row.PrevOwningTeam = 0;
        Row.CachedOwningTeamInfo = nullptr;
        Row.PointAccrualTime = 0.0f;
        Row.PointsRemainder = 0.0f;
        Row.BonusPointAccrualTime = 0.0f;
        Row.BonusPointsRemainder = 0.0f;
        Row.bPointFinished = 0;
    }

    void ReconcileDiscoControlPoints(
        AFortAthenaMutator_Disco* Disco)
    {
        if (!IsLiveObject(Disco) ||
            !Disco->HasSpawnedControlPoints())
        {
            return;
        }

        auto& Rows = Disco->SpawnedControlPoints;
        if (!IsSaneArray(Rows.Num(), Rows.Max(), 64) ||
            Rows.Num() <= 0 ||
            !IsReadableArrayStorage(
                Rows.Data,
                Rows.Num(),
                sizeof(FControlPointInstanceData)))
        {
            return;
        }

        UWorld* World = UWorld::GetWorld();
        UClass* DiscoPointClass =
            const_cast<UClass*>(FindObject<UClass>(
                L"/Game/Athena/Playlists/50v50/Disco/"
                L"DiscoPoint.DiscoPoint_C"));
        if (!World || !DiscoPointClass)
            return;

        TArray<AActor*> WorldPointArray =
            UGameplayStatics::GetAllActorsOfClass(
                World, DiscoPointClass);
        std::vector<AActor*> WorldPoints;
        if (IsSaneArray(
                WorldPointArray.Num(),
                WorldPointArray.Max(),
                64) &&
            IsReadableArrayStorage(
                WorldPointArray.Data,
                WorldPointArray.Num(),
                sizeof(AActor*)))
        {
            WorldPoints.reserve(WorldPointArray.Num());
            for (int32 Index = 0;
                 Index < WorldPointArray.Num();
                 ++Index)
            {
                AActor* Point = WorldPointArray[Index];
                if (IsLiveObject(Point) &&
                    Point->IsA(DiscoPointClass))
                {
                    WorldPoints.push_back(Point);
                }
            }
        }
        WorldPointArray.Free();

        std::unordered_map<AActor*, int32> RowReferenceCounts;
        for (int32 Index = 0; Index < Rows.Num(); ++Index)
        {
            AActor* Point = Rows[Index].ControlPoint;
            if (IsLiveObject(Point) &&
                Point->IsA(DiscoPointClass))
            {
                ++RowReferenceCounts[Point];
            }
        }

        std::vector<AActor*> AcceptedPoints;
        std::unordered_set<AActor*> UsedPoints;
        // Do not rebind an active row to an arbitrary orphan point. The row's
        // SpawnDataIdx determines its authored transform, schedule and native
        // capture delegates; swapping only the actor pointer can corrupt
        // scoring. The pre-bootstrap geometry gate prevents malformed rows in
        // a fresh match, while this audit keeps a legacy malformed match
        // observable without mutating live capture state.
        constexpr bool bMayRepairMalformedRows = false;
        int32 DuplicateRows = 0;
        int32 CoincidentRows = 0;
        int32 ReboundRows = 0;
        for (int32 Index = 0; Index < Rows.Num(); ++Index)
        {
            auto& Row = Rows[Index];
            AActor* Point = Row.ControlPoint;
            // Empty rows are normal for points whose authored spawn phase has
            // not arrived yet. Never turn those into early objectives.
            if (!IsLiveObject(Point) ||
                !Point->IsA(DiscoPointClass))
            {
                continue;
            }

            const bool bDuplicate =
                UsedPoints.contains(Point);
            bool bCoincident = false;
            if (!bDuplicate)
            {
                bCoincident = std::any_of(
                    AcceptedPoints.begin(),
                    AcceptedPoints.end(),
                    [Point](AActor* Accepted)
                    {
                        return AreDiscoPointsCoincident(
                            Point, Accepted);
                    });
            }

            DuplicateRows += bDuplicate ? 1 : 0;
            CoincidentRows += bCoincident ? 1 : 0;
            if (bDuplicate || bCoincident)
            {
                AActor* Replacement = nullptr;
                for (AActor* Candidate : WorldPoints)
                {
                    if (UsedPoints.contains(Candidate) ||
                        RowReferenceCounts.contains(Candidate))
                    {
                        continue;
                    }

                    const bool bCandidateCoincident =
                        std::any_of(
                            AcceptedPoints.begin(),
                            AcceptedPoints.end(),
                            [Candidate](AActor* Accepted)
                            {
                                return AreDiscoPointsCoincident(
                                    Candidate, Accepted);
                            });
                    if (!bCandidateCoincident)
                    {
                        Replacement = Candidate;
                        break;
                    }
                }

                if (Replacement &&
                    bMayRepairMalformedRows)
                {
                    Row.ControlPoint = Replacement;
                    ResetMalformedDiscoPointRow(Row);
                    Point = Replacement;
                    ++ReboundRows;
                }
            }

            if (!UsedPoints.contains(Point))
            {
                UsedPoints.insert(Point);
                AcceptedPoints.push_back(Point);
            }
            const bool bFirstReplicationPass =
                GAuthoredNativeLTMPhaseState
                    .DiscoReplicationPreparedPoints
                    .insert(Point)
                    .second;
            if (bFirstReplicationPass)
                PrepareDiscoPointReplication(Point);
            else
                Point->ForceNetUpdate();
        }

        const int32 MalformedRows =
            DuplicateRows + CoincidentRows;
        if (ReboundRows > 0)
        {
            // SpawnedControlPoints is native/transient, but waking its owner
            // in the same frame also flushes any replicated Disco state whose
            // actor delegates changed as a result of the repaired binding.
            Disco->FlushNetDormancy();
            Disco->ForceNetUpdate();
        }
        if (MalformedRows == 0)
        {
            GAuthoredNativeLTMPhaseState
                .DiscoMalformedPointObservations
                .erase(Disco);
            GAuthoredNativeLTMPhaseState
                .bLoggedMalformedDiscoPoints = false;
        }
        else if (!GAuthoredNativeLTMPhaseState
                      .bLoggedMalformedDiscoPoints ||
                 ReboundRows > 0)
        {
            auto& ObservationCount =
                GAuthoredNativeLTMPhaseState
                    .DiscoMalformedPointObservations[Disco];
            ObservationCount =
                ObservationCount < 2
                    ? ObservationCount + 1
                    : 2;
            GAuthoredNativeLTMPhaseState
                .bLoggedMalformedDiscoPoints = true;
            SDK::DbgLog(
                "[Disco1040] control-point audit rows=%d "
                "world=%d duplicate=%d coincident=%d "
                "rebound=%d\n",
                Rows.Num(),
                static_cast<int32>(WorldPoints.size()),
                DuplicateRows,
                CoincidentRows,
                ReboundRows);
        }
    }

    void TickAuthoredNativeLTMPhaseBridge(
        AFortGameStateAthena* GameState,
        const FNativeLTMPlaylistDescriptor* Descriptor)
    {
        if (!IsLiveObject(GameState) ||
            (!IsNativeAshtonDescriptor(Descriptor) &&
             !IsNativeDiscoDescriptor(Descriptor)))
        {
            return;
        }

        UWorld* World = UWorld::GetWorld();
        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        if (!World)
            return;
        if (PhaseState.World != World)
        {
            PhaseState = {};
            PhaseState.World = World;
        }

        auto Mutator = FindAuthoredNativeLTMMutator(
            GameState, Descriptor);
        if (!Mutator)
        {
            if (!PhaseState.bLoggedWaitingForMutator)
            {
                PhaseState.bLoggedWaitingForMutator = true;
                SDK::DbgLog(
                    "[NativeLTM] %s: waiting for the authored "
                    "%s actor before phase bootstrap\n",
                    Descriptor->LogName,
                    Descriptor->ExpectedMutatorBaseName);
            }
            return;
        }
        PhaseState.bLoggedWaitingForMutator = false;

        if (IsNativeAshtonDescriptor(Descriptor))
        {
            EnsureAshtonGameplayDependenciesLoaded();
            EnsureAshtonGameplayDelegateBindings(
                GameState, Mutator);
            const double Now =
                UGameplayStatics::GetTimeSeconds(World);
            TickAshtonLeaderPromotion(Now);
            if (Now >=
                PhaseState
                    .NextAshtonPlayerReconcileTime)
            {
                PhaseState
                    .NextAshtonPlayerReconcileTime =
                    Now + 0.50;
                ReconcileAshtonVillainPlayers(
                    GameState, "watchdog");
            }
            if (Now >=
                PhaseState
                    .NextAshtonStoneReconcileTime)
            {
                PhaseState
                    .NextAshtonStoneReconcileTime =
                    Now + 0.50;
                ReconcileAshtonStonePickups(
                    GameState,
                    static_cast<
                        AFortAthenaMutator_Ashton*>(
                            Mutator),
                    Now);
            }
            if (HasInitializedAshtonStoneRuntime(
                    Mutator))
            {
                // Setup constructs the six live stone-state rows. Seeing all
                // six unique types proves the native callback already ran;
                // replaying it would reset captures, respawns, leader state,
                // timers and intro delivery.
                PhaseState.ObservedSetup.insert(Mutator);
            }
        }

        if (IsNativeDiscoDescriptor(Descriptor) &&
            GameState->HasGamePhase())
        {
            auto Disco =
                static_cast<AFortAthenaMutator_Disco*>(
                    Mutator);
            if (Disco->HasSpawnedControlPoints())
            {
                auto& Spawned =
                    Disco->SpawnedControlPoints;
                if (IsSaneArray(
                        Spawned.Num(),
                        Spawned.Max(),
                        64) &&
                    Spawned.Num() > 0 &&
                    IsReadableArrayStorage(
                        Spawned.Data,
                        Spawned.Num(),
                        sizeof(FControlPointInstanceData)))
                {
                    // A populated native instance array is stronger proof
                    // than a late-installed observer: Setup already spawned
                    // the authored Disco floors and must not be replayed.
                    const bool bNewStepSetupProof =
                        PhaseState.ObservedSetup
                            .insert(Mutator)
                            .second;
                    const bool bNewGamePhaseSetupProof =
                        PhaseState
                            .ObservedGamePhaseSetup
                            .insert(Mutator)
                            .second;
                    if (bNewStepSetupProof)
                    {
                        PhaseState
                            .LastObservedPhaseSteps
                            .try_emplace(
                                Mutator,
                                static_cast<uint8>(
                                    EAthenaGamePhaseStep::
                                        Setup));
                    }
                    if (bNewGamePhaseSetupProof)
                    {
                        PhaseState
                            .LastObservedGamePhases
                            .try_emplace(
                                Mutator,
                                static_cast<uint8>(
                                    EAthenaGamePhase::
                                        Setup));
                    }
                }
            }

            const double Now =
                UGameplayStatics::GetTimeSeconds(World);
            if (Now >=
                PhaseState.NextDiscoGliderReconcileTime)
            {
                PhaseState.NextDiscoGliderReconcileTime =
                    Now + 1.0;
                ReconcileDiscoBigTeamGliders(GameState);
            }
            if (Now >=
                PhaseState
                    .NextDiscoControlPointReconcileTime)
            {
                PhaseState
                    .NextDiscoControlPointReconcileTime =
                    Now + DiscoControlPointReconcileInterval;
                ReconcileDiscoControlPoints(Disco);
            }

            // The native phase-step callback derives every floor transform
            // from the precomputed safe-zone centers. Replaying Setup before
            // those centers exist creates several independent rows at one
            // transform, so one dancer appears to fill multiple objectives.
            // Native callbacks remain untouched; only our missed-callback
            // recovery waits for usable geometry.
            const bool bPlaylistInputsReady =
                (GameState->HasbPlaylistDataIsLoaded() &&
                 GameState->bPlaylistDataIsLoaded) ||
                GNativeLTMCompatibilityState
                    .bAllowUnconfirmedPlaylistDataResolution;
            if (!bPlaylistInputsReady ||
                !HasReadyDiscoSafeZoneGeometry())
            {
                if (!PhaseState
                         .bLoggedWaitingForDiscoGeometry)
                {
                    PhaseState
                        .bLoggedWaitingForDiscoGeometry = true;
                    SDK::DbgLog(
                        "[Disco1040] waiting for playlist "
                        "data and initialized safe-zone "
                        "geometry before phase bootstrap\n");
                }
                return;
            }
            PhaseState.bLoggedWaitingForDiscoGeometry =
                false;

            const uint8 CurrentPhase =
                GameState->GamePhase;
            if (CurrentPhase >
                    static_cast<uint8>(
                        EAthenaGamePhase::None) &&
                CurrentPhase <
                    static_cast<uint8>(
                        EAthenaGamePhase::Count))
            {
                const uint8 SetupPhase =
                    static_cast<uint8>(
                        EAthenaGamePhase::Setup);
                if (!PhaseState
                         .ObservedGamePhaseSetup
                         .contains(Mutator) &&
                    InvokeAuthoredNativeLTMGamePhase(
                        Mutator, SetupPhase))
                {
                    SDK::DbgLog(
                        "[Disco1040] replayed missed Setup "
                        "game-phase callback on %s\n",
                        Mutator->Name.ToString().c_str());
                }

                auto Observed =
                    PhaseState.LastObservedGamePhases.find(
                        Mutator);
                if ((Observed ==
                         PhaseState
                             .LastObservedGamePhases.end() ||
                     Observed->second != CurrentPhase) &&
                    InvokeAuthoredNativeLTMGamePhase(
                        Mutator, CurrentPhase))
                {
                    SDK::DbgLog(
                        "[Disco1040] replayed missed game-phase "
                        "callback on %s phase=%u\n",
                        Mutator->Name.ToString().c_str(),
                        static_cast<unsigned>(CurrentPhase));
                }
            }
        }

        if (IsNativeAshtonDescriptor(Descriptor))
        {
            // Ashton Setup schedules the six authored stone drops from safe-
            // zone-relative spawn data. Replaying it while playlist data or
            // geometry is still incomplete permanently schedules empty/
            // invalid supply actors, even though the announcement fires.
            const bool bPlaylistInputsReady =
                (GameState->HasbPlaylistDataIsLoaded() &&
                 GameState->bPlaylistDataIsLoaded) ||
                GNativeLTMCompatibilityState
                    .bAllowUnconfirmedPlaylistDataResolution;
            if (!bPlaylistInputsReady ||
                !HasReadyAshtonSafeZoneGeometry())
            {
                if (!PhaseState
                         .bLoggedWaitingForAshtonInputs)
                {
                    PhaseState
                        .bLoggedWaitingForAshtonInputs = true;
                    SDK::DbgLog(
                        "[Ashton1040] waiting for playlist "
                        "data and initialized safe-zone "
                        "geometry before phase bootstrap\n");
                }
                return;
            }
            PhaseState.bLoggedWaitingForAshtonInputs =
                false;
        }

        if (!GameState->HasGamePhaseStep())
            return;
        const uint8 CurrentStep =
            GameState->GamePhaseStep;
        if (CurrentStep <=
                static_cast<uint8>(
                    EAthenaGamePhaseStep::None) ||
            CurrentStep >=
                static_cast<uint8>(
                    EAthenaGamePhaseStep::Count))
        {
            return;
        }

        TScriptInterface<IInterface> SafeZoneInterface{};
        if (!ResolveRelevantSafeZoneInterface(
                GameState, SafeZoneInterface))
        {
            if (!PhaseState.bLoggedWaitingForSafeZone)
            {
                PhaseState.bLoggedWaitingForSafeZone = true;
                SDK::DbgLog(
                    "[NativeLTM] %s: waiting for the relevant "
                    "safe-zone interface before phase bootstrap\n",
                    Descriptor->LogName);
            }
            return;
        }
        PhaseState.bLoggedWaitingForSafeZone = false;

        const uint8 SetupStep =
            static_cast<uint8>(
                EAthenaGamePhaseStep::Setup);
        if (!PhaseState.ObservedSetup.contains(Mutator) &&
            InvokeAuthoredNativeLTMGamePhaseStep(
                Mutator,
                SafeZoneInterface,
                SetupStep))
        {
            SDK::DbgLog(
                "[NativeLTM] %s: replayed missed Setup "
                "phase-step callback on %s\n",
                Descriptor->LogName,
                Mutator->Name.ToString().c_str());
        }

        auto Observed =
            PhaseState.LastObservedPhaseSteps.find(
                Mutator);
        if ((Observed ==
                 PhaseState.LastObservedPhaseSteps.end() ||
             Observed->second != CurrentStep) &&
            InvokeAuthoredNativeLTMGamePhaseStep(
                Mutator,
                SafeZoneInterface,
                CurrentStep))
        {
            SDK::DbgLog(
                "[NativeLTM] %s: replayed missed phase-step "
                "callback on %s step=%u\n",
                Descriptor->LogName,
                Mutator->Name.ToString().c_str(),
                static_cast<unsigned>(CurrentStep));
        }
    }

    void TickNative1040SelectedExitCraftSpawners(
        AFortGameStateAthena* GameState,
        double Now)
    {
        if (!IsLiveObject(GameState))
            return;

        // PostLoad normally installs this before gameplay. Permit two bounded
        // retries after world creation in case reflection was still settling
        // during the first attempt.
        InstallNative1040ExitCraftTimerPatch();

        if (Now <
                GHeistCompatibilityState
                    .NextExitCraftDiscoveryTime)
        {
            return;
        }
        GHeistCompatibilityState.NextExitCraftDiscoveryTime =
            Now + ExitCraftDiscoveryInterval;

        auto Heists = FindHeistMutators(GameState);
        const UStruct* EntryStruct =
            FHeistExitCraftData::StaticStruct();
        const UClass* SpawnerClass =
            AFortAthenaExitCraftSpawner::StaticClass();
        const UClass* ExitCraftClass =
            AFortAthenaExitCraft::StaticClass();
        if (Heists.empty() || !EntryStruct || !SpawnerClass ||
            !ExitCraftClass ||
            !FHeistExitCraftData::HasExitCraftSpawner() ||
            !FHeistExitCraftData::HasSpawnedExitCraft() ||
            !FHeistExitCraftData::HasExitCraftState())
        {
            GHeistCompatibilityState
                .ScheduledExitCraftSpawners.clear();
            GHeistCompatibilityState
                .Native1040DueSpawnerRecoveryTimes.clear();
            GHeistCompatibilityState
                .Native1040ManualSpawnAttempts.clear();
            GHeistCompatibilityState
                .bLoggedNative1040WaitingForSelection = false;
            return;
        }

        const int32 EntrySize = EntryStruct->GetPropertiesSize();
        if (EntrySize <= 0 || EntrySize > 0x100)
        {
            GHeistCompatibilityState
                .ScheduledExitCraftSpawners.clear();
            GHeistCompatibilityState
                .Native1040DueSpawnerRecoveryTimes.clear();
            GHeistCompatibilityState
                .Native1040ManualSpawnAttempts.clear();
            return;
        }

        bool bHasSelectedSpawner = false;
        std::unordered_set<AFortAthenaExitCraftSpawner*>
            LiveSelectedSpawners;
        auto ClearRecoveryState =
            [](AFortAthenaExitCraftSpawner* Spawner)
            {
                GHeistCompatibilityState
                    .ScheduledExitCraftSpawners.erase(Spawner);
                GHeistCompatibilityState
                    .Native1040DueSpawnerRecoveryTimes.erase(
                        Spawner);
                GHeistCompatibilityState
                    .Native1040ManualSpawnAttempts.erase(
                        Spawner);
            };
        for (auto Heist : Heists)
        {
            if (!IsLiveObject(Heist) ||
                !Heist->HasSpawnedExitCraftList())
            {
                continue;
            }

            auto& Entries = Heist->SpawnedExitCraftList;
            if (!IsSaneArray(
                    Entries.Num(), Entries.Max(), 64))
            {
                continue;
            }

            for (int32 Index = 0;
                 Index < Entries.Num();
                 ++Index)
            {
                auto& Entry = Entries.Get(Index, EntrySize);
                auto Spawner = Entry.ExitCraftSpawner;
                if (!IsLiveObject(Spawner) ||
                    !Spawner->IsA(SpawnerClass))
                {
                    continue;
                }

                bHasSelectedSpawner = true;
                LiveSelectedSpawners.insert(Spawner);
                if (IsLiveObject(Entry.SpawnedExitCraft))
                {
                    if (Entry.SpawnedExitCraft->IsA(
                            ExitCraftClass))
                    {
                        ClearRecoveryState(Spawner);
                        continue;
                    }

                    SDK::DbgLog(
                        "[Getaway1040] selected %s held a "
                        "non-exit-craft SpawnedExitCraft pointer; "
                        "clearing it for recovery\n",
                        Spawner->Name.ToString().c_str());
                    Entry.SpawnedExitCraft = nullptr;
                }

                const uint8 EntryState = Entry.ExitCraftState;
                if (EntryState == HeistExitCraftStateExited)
                {
                    ClearRecoveryState(Spawner);
                    continue;
                }
                if (EntryState != HeistExitCraftStateNone &&
                    EntryState != HeistExitCraftStateIncoming &&
                    EntryState != HeistExitCraftStateSpawned)
                {
                    ClearRecoveryState(Spawner);
                    continue;
                }

                // Spawned without a typed physical actor is not resolved; its
                // arrival deadline has already passed and it should recover
                // after only the observation grace.
                double NativeDueTime =
                    EntryState == HeistExitCraftStateSpawned
                        ? Now
                        : 0.0;
                const float EntrySpawnTime =
                    FHeistExitCraftData::HasSpawnTime()
                        ? Entry.SpawnTime
                        : 0.0f;
                if (NativeDueTime <= 0.0 &&
                    std::isfinite(EntrySpawnTime) &&
                    EntrySpawnTime > 0.0f &&
                    static_cast<double>(EntrySpawnTime) <=
                        Now +
                            Native1040MaximumTimerHorizon)
                {
                    NativeDueTime =
                        static_cast<double>(EntrySpawnTime);
                }
                const float MutatorSpawnTime =
                    Heist->HasSpawnExitCraftTime()
                        ? Heist->SpawnExitCraftTime
                        : 0.0f;
                if (NativeDueTime <= 0.0 &&
                    std::isfinite(MutatorSpawnTime) &&
                    MutatorSpawnTime > 0.0f &&
                    static_cast<double>(MutatorSpawnTime) <=
                        Now +
                            Native1040MaximumTimerHorizon)
                {
                    NativeDueTime =
                        static_cast<double>(MutatorSpawnTime);
                }
                if (NativeDueTime <= 0.0 &&
                    IsLiveObject(Spawner->ExitCraftInfo) &&
                    Spawner->ExitCraftInfo
                        ->HasExitCraftInfo() &&
                    FExitCraftInfo::StaticStruct() &&
                    FExitCraftInfo::
                        HasExitCraftSpawnDelay())
                {
                    const float ConfiguredDelay =
                        Spawner->ExitCraftInfo
                            ->ExitCraftInfo
                            .ExitCraftSpawnDelay
                            .Evaluate(0.0f);
                    if (std::isfinite(ConfiguredDelay) &&
                        ConfiguredDelay >= 0.0f &&
                        ConfiguredDelay <= 3600.0f)
                    {
                        // A stripped timer completion can leave the selected
                        // entry at None with both native timestamps zero. Use
                        // the same per-craft delay that StartExitCraftSpawnTimer
                        // uses, anchored once at first observation.
                        NativeDueTime =
                            Now +
                            static_cast<double>(
                                ConfiguredDelay);
                    }
                }
                if (NativeDueTime <= 0.0)
                {
                    // Selection itself is authoritative. An invalid/missing
                    // delay must degrade to a short guarded recovery rather
                    // than leaving three smoke markers in the world forever.
                    NativeDueTime = Now;
                }

                const double EarliestRecoveryTime =
                    (NativeDueTime > Now
                         ? NativeDueTime
                         : Now) +
                    Native1040TimerObservationGrace;
                auto Recovery =
                    GHeistCompatibilityState
                        .Native1040DueSpawnerRecoveryTimes.find(
                            Spawner);
                if (Recovery ==
                    GHeistCompatibilityState
                        .Native1040DueSpawnerRecoveryTimes.end())
                {
                    Recovery =
                        GHeistCompatibilityState
                            .Native1040DueSpawnerRecoveryTimes
                            .emplace(
                                Spawner,
                                EarliestRecoveryTime)
                            .first;
                    SDK::DbgLog(
                        "[Getaway1040] observing selected %s "
                        "state=%u now=%.2f entrySpawn=%.2f "
                        "mutatorSpawn=%.2f recovery=%.2f\n",
                        Spawner->Name.ToString().c_str(),
                        static_cast<unsigned>(EntryState),
                        Now,
                        EntrySpawnTime,
                        MutatorSpawnTime,
                        Recovery->second);
                }
                if (Now < Recovery->second)
                    continue;

                auto Scheduled =
                    GHeistCompatibilityState
                        .ScheduledExitCraftSpawners.find(
                            Spawner);
                if (Scheduled ==
                    GHeistCompatibilityState
                        .ScheduledExitCraftSpawners.end())
                {
                    // Incoming entries have already been selected and armed
                    // by the 10.40 Heist mutator. Entry.SpawnTime is that
                    // native arrival deadline. The stripped server callback
                    // is the missing operation, so restarting the reflected
                    // timer here applies ExitCraftSpawnDelay twice and leaves
                    // only the smoke/marker visible for another full cycle.
                    // Materialize the selected craft immediately after the
                    // native deadline's observation grace instead.
                    CallReflectedNoParams(
                        Spawner,
                        "DestroyBlockingActors");
                    Scheduled =
                        GHeistCompatibilityState
                            .ScheduledExitCraftSpawners
                            .emplace(Spawner, Now)
                            .first;
                    SDK::DbgLog(
                        "[Getaway1040] selected %s reached its "
                        "arrival deadline "
                        "state=%u entrySpawn=%.2f "
                        "mutatorSpawn=%.2f; materializing the "
                        "physical craft\n",
                        Spawner->Name.ToString().c_str(),
                        static_cast<unsigned>(EntryState),
                        EntrySpawnTime,
                        MutatorSpawnTime);
                }

                if (Scheduled ==
                        GHeistCompatibilityState
                            .ScheduledExitCraftSpawners.end() ||
                    Now < Scheduled->second)
                {
                    continue;
                }

                if (SpawnExitCraft(Spawner, GameState))
                {
                    ClearRecoveryState(Spawner);
                    continue;
                }

                int32& Attempts =
                    GHeistCompatibilityState
                        .Native1040ManualSpawnAttempts[Spawner];
                ++Attempts;
                if (Attempts >=
                    Native1040ManualSpawnMaxAttempts)
                {
                    Attempts = 0;
                    Recovery->second =
                        Now +
                        Native1040ManualSpawnRetryCooldown;
                    GHeistCompatibilityState
                        .ScheduledExitCraftSpawners.erase(
                            Spawner);
                    SDK::DbgLog(
                        "[Getaway1040] manual watchdog paused "
                        "selected %s after %d failures; retrying "
                        "in %.2f seconds\n",
                        Spawner->Name.ToString().c_str(),
                        Native1040ManualSpawnMaxAttempts,
                        Native1040ManualSpawnRetryCooldown);
                    continue;
                }

                Scheduled->second =
                    Now + static_cast<double>(Attempts);
                SDK::DbgLog(
                    "[Getaway1040] manual watchdog failed for "
                    "%s attempt=%d/%d; retrying in %.2f seconds\n",
                    Spawner->Name.ToString().c_str(),
                    Attempts,
                    Native1040ManualSpawnMaxAttempts,
                    static_cast<double>(Attempts));
            }
        }

        if (bHasSelectedSpawner)
        {
            GHeistCompatibilityState
                .bLoggedNative1040WaitingForSelection = false;
        }
        else if (!GHeistCompatibilityState
                      .bLoggedNative1040WaitingForSelection &&
                 GameState->HasGamePhase() &&
                 GameState->GamePhase >=
                     static_cast<uint8>(
                         EAthenaGamePhase::Aircraft))
        {
            GHeistCompatibilityState
                .bLoggedNative1040WaitingForSelection = true;
            SDK::DbgLog(
                "[Getaway1040] no selected exit-craft entries "
                "yet; candidate spawners will not be activated\n");
        }

        auto RemoveUnselectedMapEntries =
            [&LiveSelectedSpawners](auto& Entries)
            {
                for (auto Iterator = Entries.begin();
                     Iterator != Entries.end();)
                {
                    if (!LiveSelectedSpawners.contains(
                            Iterator->first))
                    {
                        Iterator = Entries.erase(Iterator);
                    }
                    else
                    {
                        ++Iterator;
                    }
                }
            };
        RemoveUnselectedMapEntries(
            GHeistCompatibilityState
                .ScheduledExitCraftSpawners);
        RemoveUnselectedMapEntries(
            GHeistCompatibilityState
                .Native1040DueSpawnerRecoveryTimes);
        RemoveUnselectedMapEntries(
            GHeistCompatibilityState
                .Native1040ManualSpawnAttempts);
    }
}

bool FFortAthenaNativeLTMCompatibility::IsSupportedBuild()
{
    return std::fabs(
        VersionInfo.FortniteVersion - NativeLTMVersion) <=
        NativeLTMVersionTolerance;
}

bool FFortAthenaNativeLTMCompatibility::IsTargetPlaylist(
    const UFortPlaylistAthena* Playlist)
{
    return IsSupportedBuild() &&
        FindExactNativeLTMDescriptor(Playlist) != nullptr;
}

bool FFortAthenaNativeLTMCompatibility::IsReadyForMatch(
    AFortGameStateAthena* GameState,
    const UFortPlaylistAthena* Playlist)
{
    if (!IsTargetPlaylist(Playlist))
        return true;

    UWorld* World = UWorld::GetWorld();
    return World && GameState &&
        GNativeLTMCompatibilityState.World == World &&
        GNativeLTMCompatibilityState.Playlist == Playlist &&
        GNativeLTMCompatibilityState.bCompatibilityComplete;
}

void FFortAthenaNativeLTMCompatibility::BeginPlaylistPublication(
    AFortGameStateAthena* GameState,
    const UFortPlaylistAthena* Playlist)
{
    UWorld* World = UWorld::GetWorld();
    const auto Descriptor =
        IsSupportedBuild()
            ? FindExactNativeLTMDescriptor(Playlist)
            : nullptr;
    if (!World || !GameState || !Descriptor)
        return;

    if (GNativeLTMCompatibilityState.World != World ||
        GNativeLTMCompatibilityState.Playlist != Playlist)
    {
        ResetNativeLTMCompatibilityState(
            World, Playlist, Descriptor);
        if (IsNativeGetawayDescriptor(Descriptor))
            ResetHeistCompatibilityState(World, Playlist);
    }
    GNativeLTMCompatibilityState
        .bPlaylistPublicationInProgress = true;
}

void FFortAthenaNativeLTMCompatibility::EndPlaylistPublication(
    AFortGameStateAthena* GameState,
    const UFortPlaylistAthena* Playlist)
{
    UWorld* World = UWorld::GetWorld();
    if (!World || !GameState ||
        GNativeLTMCompatibilityState.World != World ||
        GNativeLTMCompatibilityState.Playlist != Playlist)
    {
        return;
    }

    GNativeLTMCompatibilityState
        .bPlaylistPublicationInProgress = false;
    GNativeLTMCompatibilityState
        .bPlaylistPublicationCompleted = true;
}

void FFortAthenaNativeLTMCompatibility::PreparePlaylist(
    AFortGameStateAthena* GameState,
    const UFortPlaylistAthena* Playlist)
{
    UWorld* World = UWorld::GetWorld();
    const auto Descriptor =
        IsSupportedBuild()
            ? FindExactNativeLTMDescriptor(Playlist)
            : nullptr;
    if (!World || !GameState || !Descriptor)
        return;

    if (GNativeLTMCompatibilityState.World != World ||
        GNativeLTMCompatibilityState.Playlist != Playlist)
    {
        ResetNativeLTMCompatibilityState(
            World, Playlist, Descriptor);
        if (IsNativeGetawayDescriptor(Descriptor))
            ResetHeistCompatibilityState(World, Playlist);
    }
    if (GNativeLTMCompatibilityState.bDefinitionsPrepared)
        return;
    const bool bPlaylistDataLoaded =
        GameState->HasbPlaylistDataIsLoaded() &&
        GameState->bPlaylistDataIsLoaded;
    if (!bPlaylistDataLoaded &&
        !GNativeLTMCompatibilityState
             .bAllowUnconfirmedPlaylistDataResolution)
    {
        if (!GNativeLTMCompatibilityState
                 .bLoggedWaitingForPlaylistData)
        {
            GNativeLTMCompatibilityState
                .bLoggedWaitingForPlaylistData = true;
            SDK::DbgLog(
                "[NativeLTM] %s: waiting for "
                "bPlaylistDataIsLoaded before resolving mutators\n",
                Descriptor->LogName);
        }
        return;
    }

    const UClass* GameplayMutatorClass =
        AFortGameplayMutator::StaticClass();
    const UClass* ExpectedMutatorBase =
        FindClass(Descriptor->ExpectedMutatorBaseName);
    bool bComplete =
        GameplayMutatorClass && ExpectedMutatorBase &&
        Playlist->HasModifierList();
    bool bFoundExpectedMutator = false;
    int32 ResolvedModifierCount = 0;
    int32 DeclaredMutatorCount = 0;
    std::vector<UFortGameplayModifierItemDefinition*>
        ResolvedModifiers;
    std::vector<FResolvedNativeLTMMutator> ResolvedMutators;

    if (Playlist->HasModifierList())
    {
        auto& ModifierList =
            const_cast<UFortPlaylistAthena*>(Playlist)
                ->ModifierList;
        if (!IsSaneArray(
                ModifierList.Num(), ModifierList.Max(), 64) ||
            ModifierList.Num() == 0 ||
            !IsReadableArrayStorage(
                ModifierList.Data,
                ModifierList.Num(),
                FSoftObjectPtr::Size()))
        {
            bComplete = false;
        }
        else
        {
            for (int32 ModifierIndex = 0;
                 ModifierIndex < ModifierList.Num();
                 ++ModifierIndex)
            {
                auto& ModifierDefinition =
                    ModifierList.Get(
                        ModifierIndex,
                        FSoftObjectPtr::Size());
                auto Modifier =
                    const_cast<
                        UFortGameplayModifierItemDefinition*>(
                        ModifierDefinition.Get());
                if (!Modifier &&
                    GNativeLTMCompatibilityState
                        .bAllowUnconfirmedPlaylistDataResolution &&
                    ModifierDefinition.ObjectID
                        .AssetPathName.IsValid())
                {
                    const auto ModifierPath =
                        ModifierDefinition.ObjectID
                            .AssetPathName.ToWString();
                    Modifier = const_cast<
                        UFortGameplayModifierItemDefinition*>(
                        FindObject<
                            UFortGameplayModifierItemDefinition>(
                            ModifierPath.c_str()));
                }
                if (!IsLiveObject(Modifier))
                {
                    bComplete = false;
                    continue;
                }

                Modifier->AddToRoot();
                ++ResolvedModifierCount;
                if (std::find(
                        ResolvedModifiers.begin(),
                        ResolvedModifiers.end(),
                        Modifier) ==
                    ResolvedModifiers.end())
                {
                    ResolvedModifiers.push_back(Modifier);
                }
                if (!Modifier->HasMutators())
                {
                    bComplete = false;
                    continue;
                }

                auto& MutatorDefinitions =
                    Modifier->Mutators;
                if (!IsSaneArray(
                        MutatorDefinitions.Num(),
                        MutatorDefinitions.Max(),
                        64) ||
                    !IsReadableArrayStorage(
                        MutatorDefinitions.Data,
                        MutatorDefinitions.Num(),
                        FSoftObjectPtr::Size()))
                {
                    bComplete = false;
                    continue;
                }

                for (int32 MutatorIndex = 0;
                     MutatorIndex <
                         MutatorDefinitions.Num();
                     ++MutatorIndex)
                {
                    ++DeclaredMutatorCount;
                    auto& MutatorDefinition =
                        MutatorDefinitions.Get(
                            MutatorIndex,
                            FSoftObjectPtr::Size());
                    UClass* MutatorClass =
                        MutatorDefinition.Get();
                    if (!MutatorClass &&
                        GNativeLTMCompatibilityState
                            .bAllowUnconfirmedPlaylistDataResolution &&
                        MutatorDefinition.ObjectID
                            .AssetPathName.IsValid())
                    {
                        const auto MutatorPath =
                            MutatorDefinition.ObjectID
                                .AssetPathName.ToWString();
                        MutatorClass =
                            const_cast<UClass*>(
                                FindObject<UClass>(
                                    MutatorPath.c_str()));
                    }
                    const UObject* DefaultObject =
                        MutatorClass
                            ? MutatorClass->GetDefaultObj()
                            : nullptr;
                    if (!IsLiveObject(MutatorClass) ||
                        !IsLiveObject(DefaultObject) ||
                        !DefaultObject->IsA(
                            GameplayMutatorClass))
                    {
                        bComplete = false;
                        continue;
                    }

                    MutatorClass->AddToRoot();
                    if (DefaultObject->IsA(
                            ExpectedMutatorBase))
                    {
                        bFoundExpectedMutator = true;
                    }

                    bool bMutatesGameMode = true;
                    bool bMutatesGameState = true;
                    const UClass* AthenaMutatorClass =
                        AFortAthenaMutator::StaticClass();
                    if (AthenaMutatorClass &&
                        DefaultObject->IsA(
                            AthenaMutatorClass))
                    {
                        const auto AthenaDefault =
                            static_cast<
                                const AFortAthenaMutator*>(
                                DefaultObject);
                        if (!AthenaDefault
                                 ->HasbMutatesGameMode() ||
                            !AthenaDefault
                                 ->HasbMutatesGameState())
                        {
                            bComplete = false;
                            continue;
                        }
                        bMutatesGameMode =
                            AthenaDefault->bMutatesGameMode;
                        bMutatesGameState =
                            AthenaDefault->bMutatesGameState;
                    }

                    const bool bAlreadyResolved =
                        std::any_of(
                            ResolvedMutators.begin(),
                            ResolvedMutators.end(),
                            [&](const auto& Existing)
                            {
                                return Existing.Class ==
                                    MutatorClass;
                            });
                    if (!bAlreadyResolved)
                    {
                        ResolvedMutators.emplace_back(
                            Modifier,
                            MutatorDefinition,
                            MutatorClass,
                            bMutatesGameMode,
                            bMutatesGameState);
                    }
                }
            }
        }
    }

    bComplete =
        bComplete &&
        ResolvedModifierCount > 0 &&
        DeclaredMutatorCount > 0 &&
        !ResolvedMutators.empty() &&
        bFoundExpectedMutator;
    if (!bComplete)
    {
        if (!GNativeLTMCompatibilityState
                 .bLoggedResolutionFailure)
        {
            GNativeLTMCompatibilityState
                .bLoggedResolutionFailure = true;
            SDK::DbgLog(
                "[NativeLTM] %s: configured mutators are not "
                "ready/valid (modifiers=%d declared=%d "
                "resolved=%llu expectedBase=%s found=%d)\n",
                Descriptor->LogName,
                ResolvedModifierCount,
                DeclaredMutatorCount,
                static_cast<unsigned long long>(
                    ResolvedMutators.size()),
                Descriptor->ExpectedMutatorBaseName,
                bFoundExpectedMutator ? 1 : 0);
        }
        return;
    }

    GNativeLTMCompatibilityState.Modifiers =
        std::move(ResolvedModifiers);
    GNativeLTMCompatibilityState.Mutators =
        std::move(ResolvedMutators);
    GNativeLTMCompatibilityState.bDefinitionsPrepared = true;
    SDK::DbgLog(
        "[NativeLTM] %s: rooted %d modifier definitions "
        "and %llu configured mutator classes\n",
        Descriptor->LogName,
        ResolvedModifierCount,
        static_cast<unsigned long long>(
            GNativeLTMCompatibilityState.Mutators.size()));
    for (const auto& Mutator :
         GNativeLTMCompatibilityState.Mutators)
    {
        SDK::DbgLog(
            "[NativeLTM] %s: configured class=%s "
            "mutatesGameMode=%d mutatesGameState=%d\n",
            Descriptor->LogName,
            Mutator.Class->Name.ToString().c_str(),
            Mutator.bMutatesGameMode ? 1 : 0,
            Mutator.bMutatesGameState ? 1 : 0);
    }
}

void FFortAthenaNativeLTMCompatibility::Tick(
    UNetDriver* Driver,
    float DeltaSeconds)
{
    (void)DeltaSeconds;
    if (!IsSupportedBuild() || !Driver)
        return;

    UWorld* World = UWorld::GetWorld();
    if (!World || Driver != World->NetDriver ||
        !World->GameState)
    {
        return;
    }

    auto GameState =
        World->GameState->Cast<AFortGameStateAthena>();
    if (!GameState)
        return;

    const UFortPlaylistAthena* Playlist = nullptr;
    const FNativeLTMPlaylistDescriptor* Descriptor = nullptr;
    if (GameState->HasCurrentPlaylistInfo())
    {
        if (FPlaylistPropertyArray::HasOverridePlaylist())
        {
            const auto Candidate =
                GameState->CurrentPlaylistInfo.OverridePlaylist;
            if (const auto CandidateDescriptor =
                    FindExactNativeLTMDescriptor(Candidate))
            {
                Playlist = Candidate;
                Descriptor = CandidateDescriptor;
            }
        }
        if (!Playlist &&
            FPlaylistPropertyArray::HasBasePlaylist())
        {
            const auto Candidate =
                GameState->CurrentPlaylistInfo.BasePlaylist;
            if (const auto CandidateDescriptor =
                    FindExactNativeLTMDescriptor(Candidate))
            {
                Playlist = Candidate;
                Descriptor = CandidateDescriptor;
            }
        }
    }
    if (!Playlist && GameState->HasCurrentPlaylistData())
    {
        const auto Candidate =
            GameState->CurrentPlaylistData;
        if (const auto CandidateDescriptor =
                FindExactNativeLTMDescriptor(Candidate))
        {
            Playlist = Candidate;
            Descriptor = CandidateDescriptor;
        }
    }
    if (!Playlist || !Descriptor)
        return;

    if (GNativeLTMCompatibilityState.World != World ||
        GNativeLTMCompatibilityState.Playlist != Playlist)
    {
        ResetNativeLTMCompatibilityState(
            World, Playlist, Descriptor);
        if (IsNativeGetawayDescriptor(Descriptor))
            ResetHeistCompatibilityState(World, Playlist);
        SDK::DbgLog(
            "[NativeLTM] selected exact 10.40 playlist: %s\n",
            Descriptor->LogName);
    }
    if (IsNativeFoodFightDescriptor(Descriptor))
        TickDeepFriedArena(GameState);

    if (GNativeLTMCompatibilityState
            .bPlaylistPublicationInProgress)
    {
        // OnRep_CurrentPlaylistInfo pumps the NetDriver while it creates and
        // registers the playlist's mutators.  Never run the non-idempotent
        // compatibility registration path from that re-entrant tick.
        return;
    }
    if (!GNativeLTMCompatibilityState
             .bPlaylistPublicationCompleted)
    {
        // FortGameMode intentionally waits for MapInfo before publishing the
        // playlist.  Do not let the compatibility fallback register the same
        // gameplay modifiers while that authoritative publication is pending.
        return;
    }

    const double Now =
        UGameplayStatics::GetTimeSeconds(World);
    if (IsNativeGetawayDescriptor(Descriptor))
    {
        // 10.40's Heist phase callback takes the safe-zone interface plus the
        // phase step, unlike the one-byte 5.41 ABI. Replay only a phase the
        // hook proves this mutator missed, allowing native code to choose the
        // correct van spawners. Then start/watch only those selected entries:
        // scanning every world spawner would turn all candidate minimap
        // markers into vans.
        if (GHeistCompatibilityState.World != World ||
            GHeistCompatibilityState.Playlist != Playlist)
        {
            ResetHeistCompatibilityState(World, Playlist);
        }
        TickNative1040HeistPhaseBridge(GameState, Now);
        TickNative1040SelectedExitCraftSpawners(
            GameState, Now);
    }
    if (IsNativeArsenalDescriptor(Descriptor))
        TickArsenalCompatibility(GameState, Now);
    if (IsNativeWaxDescriptor(Descriptor))
        TickWaxCompatibility(GameState, Now);
    if (IsNativeAshtonDescriptor(Descriptor) ||
        IsNativeDiscoDescriptor(Descriptor))
    {
        TickAuthoredNativeLTMPhaseBridge(
            GameState, Descriptor);
    }

    if (GNativeLTMCompatibilityState.bCompatibilityComplete)
        return;

    const bool bPlaylistDataLoaded =
        GameState->HasbPlaylistDataIsLoaded() &&
        GameState->bPlaylistDataIsLoaded;
    if (!bPlaylistDataLoaded)
    {
        PreparePlaylist(GameState, Playlist);

        const bool bPlaylistDataLoading =
            GameState->HasbPlaylistDataIsActivelyLoading() &&
            GameState->bPlaylistDataIsActivelyLoading;
        if (bPlaylistDataLoading)
        {
            GNativeLTMCompatibilityState
                .NextPlaylistRepublishTime =
                Now + NativeLTMAttemptInterval;
            return;
        }

        if (GNativeLTMCompatibilityState
                .PlaylistRepublishAttempts >=
            NativeLTMMaxPlaylistRepublishAttempts)
        {
            auto& State = GNativeLTMCompatibilityState;
            if (State.NextPlaylistRepublishTime >= 0.0 &&
                Now < State.NextPlaylistRepublishTime)
            {
                return;
            }
            State.NextPlaylistRepublishTime =
                Now + NativeLTMAttemptInterval;

            if (!State
                     .bAllowUnconfirmedPlaylistDataResolution)
            {
                State
                    .bAllowUnconfirmedPlaylistDataResolution =
                    true;
                State.bLoggedResolutionFailure = false;
                if (!State
                         .bLoggedUnconfirmedPlaylistDataFallback)
                {
                    State
                        .bLoggedUnconfirmedPlaylistDataFallback =
                        true;
                    SDK::DbgLog(
                        "[NativeLTM] %s: playlist-data flag "
                        "remained false after %d observations; "
                        "resolving resident modifier assets "
                        "without writing the native load flag\n",
                        Descriptor->LogName,
                        NativeLTMMaxPlaylistRepublishAttempts);
                }
            }

            PreparePlaylist(GameState, Playlist);
            if (!State.bDefinitionsPrepared)
            {
                if (!State.bLoggedResolutionFailure)
                {
                    State.bLoggedResolutionFailure = true;
                    SDK::DbgLog(
                        "[NativeLTM] %s: modifier resolution "
                        "is still pending after playlist-data "
                        "observations; it will be retried without "
                        "writing the native load flag\n",
                        Descriptor->LogName);
                }
                return;
            }
        }
        else
        {
            if (GNativeLTMCompatibilityState
                    .NextPlaylistRepublishTime < 0.0)
            {
                GNativeLTMCompatibilityState
                    .NextPlaylistRepublishTime =
                    Now + NativeLTMAttemptInterval;
                return;
            }
            if (Now <
                GNativeLTMCompatibilityState
                    .NextPlaylistRepublishTime)
            {
                return;
            }

            ++GNativeLTMCompatibilityState
                  .PlaylistRepublishAttempts;
            GNativeLTMCompatibilityState
                .NextPlaylistRepublishTime =
                Now + NativeLTMAttemptInterval;
            SDK::DbgLog(
                "[NativeLTM] %s: playlist-data initialization "
                "observation %d/%d; not replaying non-idempotent "
                "playlist callbacks\n",
                Descriptor->LogName,
                GNativeLTMCompatibilityState
                    .PlaylistRepublishAttempts,
                NativeLTMMaxPlaylistRepublishAttempts);
            return;
        }
    }

    if (GNativeLTMCompatibilityState
            .PlaylistDataLoadedTime < 0.0)
    {
        GNativeLTMCompatibilityState.PlaylistDataLoadedTime =
            Now;
        GNativeLTMCompatibilityState.NextAttemptTime = Now;
    }

    PreparePlaylist(GameState, Playlist);
    if (!GNativeLTMCompatibilityState.bDefinitionsPrepared)
        return;

    auto GameMode = World->AuthorityGameMode
        ? World->AuthorityGameMode->Cast<AFortGameMode>()
        : nullptr;
    auto GameModeComponent =
        GetMutatorListComponent(GameMode);
    auto GameStateComponent =
        GetMutatorListComponent(GameState);

    if (NativeMutatorSetIsComplete(
            GameMode,
            GameState,
            GameModeComponent,
            GameStateComponent))
    {
        GNativeLTMCompatibilityState
            .bCompatibilityComplete = true;
        SDK::DbgLog(
            "[NativeLTM] %s: native playlist pipeline "
            "registered and activated all configured mutators\n",
            Descriptor->LogName);
        return;
    }

    const double GraceEnd =
        GNativeLTMCompatibilityState.PlaylistDataLoadedTime +
        NativeLTMInitializationGraceSeconds;
    if (Now < GraceEnd)
    {
        if (!GNativeLTMCompatibilityState
                 .bLoggedNativeGracePeriod)
        {
            GNativeLTMCompatibilityState
                .bLoggedNativeGracePeriod = true;
            SDK::DbgLog(
                "[NativeLTM] %s: allowing native mutator "
                "initialization %.1f seconds before fallback\n",
                Descriptor->LogName,
                NativeLTMInitializationGraceSeconds);
        }
        return;
    }

    if (Now <
        GNativeLTMCompatibilityState.NextAttemptTime)
    {
        return;
    }
    GNativeLTMCompatibilityState.NextAttemptTime =
        Now + NativeLTMAttemptInterval;

    if (!GameMode || !GameModeComponent ||
        !GameStateComponent)
    {
        if (!GNativeLTMCompatibilityState
                 .bLoggedMissingRuntimeObjects)
        {
            GNativeLTMCompatibilityState
                .bLoggedMissingRuntimeObjects = true;
            SDK::DbgLog(
                "[NativeLTM] %s: fallback deferred; "
                "GameMode=%p GameModeComponent=%p "
                "GameStateComponent=%p\n",
                Descriptor->LogName,
                static_cast<void*>(GameMode),
                static_cast<void*>(GameModeComponent),
                static_cast<void*>(GameStateComponent));
        }
        return;
    }

    bool bInvokedNativeModifierRegistration = false;
    bool bWaitingForNativeModifierRegistration = false;
    bool bNativeModifierRegistrationFailed = false;
    for (auto Modifier :
         GNativeLTMCompatibilityState.Modifiers)
    {
        const auto Lookup =
            FindActiveGameplayModifier(
                GameState, Modifier);
        if (Lookup ==
            EActiveGameplayModifierLookup::Found)
        {
            continue;
        }
        if (Lookup ==
            EActiveGameplayModifierLookup::Unavailable)
        {
            // RegisterGameplayModifier is not idempotent. If neither the
            // reflected query nor the validated native array can confirm that
            // this definition is absent, do not risk applying its persistent
            // effects or spawning its mutators twice.
            if (!GNativeLTMCompatibilityState
                     .bLoggedModifierLookupUnavailable)
            {
                GNativeLTMCompatibilityState
                    .bLoggedModifierLookupUnavailable = true;
                SDK::DbgLog(
                    "[NativeLTM] %s: modifier registration deferred: "
                    "active gameplay modifiers could not be queried "
                    "safely; refusing duplicate-prone registration "
                    "for modifier=%s\n",
                    Descriptor->LogName,
                    Modifier->Name.ToString().c_str());
            }
            return;
        }

        const auto ExistingAttempt =
            GNativeLTMCompatibilityState
                .NativeModifierRegistrationAttempts
                .find(Modifier);
        if (ExistingAttempt ==
            GNativeLTMCompatibilityState
                .NativeModifierRegistrationAttempts
                .end())
        {
            const bool bInvoked =
                InvokeRegisterGameplayModifier(
                    GameState, Modifier);
            GNativeLTMCompatibilityState
                .NativeModifierRegistrationAttempts
                .emplace(
                    Modifier,
                    bInvoked ? Now : -1.0);
            if (!bInvoked)
            {
                bNativeModifierRegistrationFailed = true;
                SDK::DbgLog(
                    "[NativeLTM] %s: reflected "
                    "RegisterGameplayModifier unavailable "
                    "for modifier=%s\n",
                    Descriptor->LogName,
                    Modifier->Name.ToString().c_str());
                continue;
            }

            bInvokedNativeModifierRegistration = true;
            bWaitingForNativeModifierRegistration = true;
            SDK::DbgLog(
                "[NativeLTM] %s: invoked native "
                "RegisterGameplayModifier modifier=%s "
                "expiration=0\n",
                Descriptor->LogName,
                Modifier->Name.ToString().c_str());
            continue;
        }

        if (ExistingAttempt->second >= 0.0 &&
            Now - ExistingAttempt->second <
                NativeLTMAttemptInterval)
        {
            bWaitingForNativeModifierRegistration = true;
        }
        else
        {
            bNativeModifierRegistrationFailed = true;
        }
    }

    if (NativeMutatorSetIsComplete(
            GameMode,
            GameState,
            GameModeComponent,
            GameStateComponent))
    {
        GNativeLTMCompatibilityState
            .bCompatibilityComplete = true;
        SDK::DbgLog(
            "[NativeLTM] %s: native gameplay-modifier "
            "registration restored all configured mutators\n",
            Descriptor->LogName);
        return;
    }
    if (bInvokedNativeModifierRegistration ||
        bWaitingForNativeModifierRegistration)
    {
        return;
    }

    if (!bNativeModifierRegistrationFailed)
    {
        // All modifier records were already present, but the native actor
        // set was still incomplete after the initial three-second grace.
        bNativeModifierRegistrationFailed = true;
    }

    // Actor-only fallback cannot recreate modifier-owned persistent gameplay
    // effects or ability sets. Only repair the actor/list side after every
    // configured modifier has an authoritative active record.
    if (!AllGameplayModifiersAreRegistered(GameState))
    {
        if (!GNativeLTMCompatibilityState
                 .bLoggedModifierEffectsUnavailable)
        {
            GNativeLTMCompatibilityState
                .bLoggedModifierEffectsUnavailable = true;
            SDK::DbgLog(
                "[NativeLTM] %s: configured-class fallback "
                "deferred because active modifier records (and "
                "their persistent effects) are incomplete\n",
                Descriptor->LogName);
        }
        return;
    }

    if (!GNativeLTMCompatibilityState
             .bLoggedManualFallback)
    {
        GNativeLTMCompatibilityState
            .bLoggedManualFallback = true;
        SDK::DbgLog(
            "[NativeLTM] %s: native "
            "RegisterGameplayModifier was unavailable or "
            "did not publish an active modifier; starting "
            "configured-class fallback\n",
            Descriptor->LogName);
    }

    bool bAllMutatorsReady = true;
    for (const auto& Desired :
         GNativeLTMCompatibilityState.Mutators)
    {
        auto Mutator = FindExistingMutator(
            GameMode,
            GameState,
            GameModeComponent,
            GameStateComponent,
            Desired);
        const bool bWasNative = Mutator != nullptr;
        if (!Mutator)
        {
            Mutator = SpawnConfiguredMutator(
                World, GameMode, GameState, Desired);
        }
        if (!Mutator)
        {
            bAllMutatorsReady = false;
            SDK::DbgLog(
                "[NativeLTM] %s: failed to %s class=%s\n",
                Descriptor->LogName,
                bWasNative ? "resolve" : "spawn",
                Desired.Class->Name.ToString().c_str());
            continue;
        }

        bool bGameModeDirectMutation = false;
        bool bGameStateDirectMutation = false;
        const bool bRegisteredWithGameMode =
            !Desired.bMutatesGameMode ||
            RegisterMutator(
                GameMode,
                GameModeComponent,
                Desired,
                Mutator,
                &bGameModeDirectMutation);
        const bool bRegisteredWithGameState =
            !Desired.bMutatesGameState ||
            RegisterMutator(
                GameState,
                GameStateComponent,
                Desired,
                Mutator,
                &bGameStateDirectMutation);
        const bool bActive = SetMutatorActive(Mutator);
        if (!bRegisteredWithGameMode ||
            !bRegisteredWithGameState ||
            !bActive)
        {
            bAllMutatorsReady = false;
            SDK::DbgLog(
                "[NativeLTM] %s: class=%s source=%s "
                "registration/activation failed "
                "(gm=%d gs=%d active=%d)\n",
                Descriptor->LogName,
                Desired.Class->Name.ToString().c_str(),
                bWasNative ? "native" : "fallback",
                bRegisteredWithGameMode ? 1 : 0,
                bRegisteredWithGameState ? 1 : 0,
                bActive ? 1 : 0);
            continue;
        }

        SDK::DbgLog(
            "[NativeLTM] %s: class=%s source=%s "
            "registered/active (gm=%d gs=%d "
            "directGM=%d directGS=%d)\n",
            Descriptor->LogName,
            Desired.Class->Name.ToString().c_str(),
            bWasNative ? "native" : "fallback",
            Desired.bMutatesGameMode ? 1 : 0,
            Desired.bMutatesGameState ? 1 : 0,
            bGameModeDirectMutation ? 1 : 0,
            bGameStateDirectMutation ? 1 : 0);
    }

    if (bAllMutatorsReady &&
        AllGameplayModifiersAreRegistered(GameState) &&
        NativeMutatorActorsAreComplete(
            GameMode,
            GameState,
            GameModeComponent,
            GameStateComponent))
    {
        GNativeLTMCompatibilityState
            .bCompatibilityComplete = true;
        SDK::DbgLog(
            "[NativeLTM] %s: fallback complete; "
            "configured mutators=%llu gmCount=%d gsCount=%d\n",
            Descriptor->LogName,
            static_cast<unsigned long long>(
                GNativeLTMCompatibilityState.Mutators.size()),
            GameModeComponent->Mutators.Num(),
            GameStateComponent->Mutators.Num());
    }
}

bool FFortAthenaHeistCompatibility::IsSupportedBuild()
{
    return VersionInfo.FortniteVersion >= HeistMinimumVersion &&
        VersionInfo.FortniteVersion < HeistEndVersionExclusive;
}

bool FFortAthenaHeistCompatibility::IsHeistPlaylist(
    const UFortPlaylistAthena* Playlist)
{
    if (!IsSupportedBuild() || !Playlist ||
        !AFortAthenaMutator_Heist::StaticClass())
    {
        return false;
    }

    if (FConfiguration::Playlist)
    {
        const std::wstring ConfiguredPath =
            FConfiguration::Playlist;
        if (IsHeistPlaylistIdentifier(ConfiguredPath.c_str()))
        {
            // SetupPlaylist falls back to DefaultSolo when the configured
            // asset cannot be resolved. Never let the configured path alone
            // turn that unrelated fallback object into a Heist playlist.
            static std::wstring CachedConfiguredPath;
            static const UFortPlaylistAthena*
                CachedConfiguredPlaylist =
                nullptr;
            static bool bConfiguredLookupAttempted = false;
            if (!bConfiguredLookupAttempted ||
                CachedConfiguredPath != ConfiguredPath)
            {
                bConfiguredLookupAttempted = true;
                CachedConfiguredPath = ConfiguredPath;
                CachedConfiguredPlaylist =
                    FindObject<UFortPlaylistAthena>(
                        FConfiguration::Playlist);
            }
            auto ConfiguredPlaylist = CachedConfiguredPlaylist;
            if (ConfiguredPlaylist == Playlist)
                return true;
        }
    }

    const auto ObjectName = Playlist->Name.ToWString();
    if (IsHeistPlaylistIdentifier(ObjectName.c_str()))
    {
        return true;
    }

    if (Playlist->HasPlaylistName())
    {
        const auto PlaylistName =
            Playlist->PlaylistName.ToWString();
        if (IsHeistPlaylistIdentifier(PlaylistName.c_str()))
        {
            return true;
        }
    }

    if (!Playlist->HasModifierList())
        return false;

    auto& ModifierList =
        const_cast<UFortPlaylistAthena*>(Playlist)->ModifierList;
    if (!IsSaneArray(
            ModifierList.Num(), ModifierList.Max(), 32))
    {
        return false;
    }

    const UClass* HeistClass =
        AFortAthenaMutator_Heist::StaticClass();
    for (int32 ModifierIndex = 0;
         ModifierIndex < ModifierList.Num(); ++ModifierIndex)
    {
        auto Modifier =
            ModifierList.Get(
                ModifierIndex, FSoftObjectPtr::Size()).Get();
        if (!Modifier || !Modifier->HasMutators())
            continue;

        auto& Mutators =
            const_cast<UFortGameplayModifierItemDefinition*>(Modifier)
                ->Mutators;
        if (!IsSaneArray(Mutators.Num(), Mutators.Max(), 32))
            continue;

        for (int32 MutatorIndex = 0;
             MutatorIndex < Mutators.Num(); ++MutatorIndex)
        {
            UClass* MutatorClass =
                Mutators.Get(
                    MutatorIndex, FSoftObjectPtr::Size()).Get();
            if (!MutatorClass)
                continue;
            const UObject* DefaultObject =
                MutatorClass->GetDefaultObj();
            if (MutatorClass == HeistClass ||
                (DefaultObject && DefaultObject->IsA(HeistClass)))
            {
                return true;
            }
        }
    }

    return false;
}

void FFortAthenaHeistCompatibility::PreparePlaylist(
    AFortGameStateAthena* GameState,
    const UFortPlaylistAthena* Playlist)
{
    UWorld* World = UWorld::GetWorld();
    if (!IsSupportedBuild() || !World || !GameState ||
        !IsHeistPlaylist(Playlist))
    {
        return;
    }

    if (GHeistCompatibilityState.World != World ||
        GHeistCompatibilityState.Playlist != Playlist)
    {
        ResetHeistCompatibilityState(World, Playlist);
    }
    if (GHeistCompatibilityState.bPlaylistPrepared)
        return;

    const bool bLoaded =
        GameState->HasbPlaylistDataIsLoaded() &&
        GameState->bPlaylistDataIsLoaded;
    const bool bLoading =
        GameState->HasbPlaylistDataIsActivelyLoading() &&
        GameState->bPlaylistDataIsActivelyLoading;
    if (bLoaded)
    {
        GHeistCompatibilityState.bPlaylistPrepared = true;
        SDK::DbgLog(
            "[Heist] playlist data already loaded; "
            "skipping duplicate load\n");
        return;
    }
    if (bLoading)
        return;

    UFunction* Initialize =
        GameState->GetFunction(
            "InitializePlaylistDataPreDataLoad");
    UFunction* Load =
        GameState->GetFunction("LoadCurrentPlaylistData");
    if (HasNoParameters(Initialize) &&
        HasNoParameters(Load))
    {
        GameState->Call<void>(Initialize);
        GameState->Call<void>(Load);
        GHeistCompatibilityState.bPlaylistPrepared = true;
        SDK::DbgLog(
            "[Heist] invoked reflected playlist initialization "
            "pipeline\n");
        return;
    }

    const uintptr_t NativeInitialize =
        FindNativeInitializePlaylistDataPreDataLoad();
    const uintptr_t NativeLoad =
        FindNativeLoadCurrentPlaylistData();
    if (!NativeInitialize || !NativeLoad)
    {
        if (!GHeistCompatibilityState
                 .bLoggedMissingPlaylistLoadFunctions)
        {
            GHeistCompatibilityState
                .bLoggedMissingPlaylistLoadFunctions = true;
            SDK::DbgLog(
                "[Heist] reflected playlist loaders unavailable "
                "(init=%p load=%p); validated native fallback "
                "incomplete (init=%p load=%p)\n",
                static_cast<void*>(Initialize),
                static_cast<void*>(Load),
                reinterpret_cast<void*>(NativeInitialize),
                reinterpret_cast<void*>(NativeLoad));
        }
        return;
    }

    using PlaylistDataFunction = void(*)(AFortGameStateAthena*);
    reinterpret_cast<PlaylistDataFunction>(NativeInitialize)(GameState);
    reinterpret_cast<PlaylistDataFunction>(NativeLoad)(GameState);
    GHeistCompatibilityState.bPlaylistPrepared = true;
    SDK::DbgLog(
        "[Heist] invoked validated native playlist initialization "
        "pipeline\n");
}

bool FFortAthenaHeistCompatibility::LoadAdditionalPlaylistLevels(
    AFortGameStateAthena* GameState,
    const UFortPlaylistAthena* Playlist)
{
    UWorld* World = UWorld::GetWorld();
    if (!IsSupportedBuild() || !World || !GameState ||
        !IsHeistPlaylist(Playlist))
    {
        return false;
    }

    if (GHeistCompatibilityState.World != World ||
        GHeistCompatibilityState.Playlist != Playlist)
    {
        ResetHeistCompatibilityState(World, Playlist);
    }
    if (GHeistCompatibilityState.bAdditionalLevelsComplete)
        return true;

    const UClass* StreamingClass =
        ULevelStreamingDynamic::StaticClass();
    if (!StreamingClass)
    {
        SDK::DbgLog(
            "[Heist] LevelStreamingDynamic class unavailable\n");
        return false;
    }

    int32 RequestedLevels = 0;
    int32 LoadedLevels = 0;
    int32 DeclaredLevels = 0;
    int32 ValidLevelEntries = 0;
    bool bInvalidLevelArray = false;
    auto LoadLevels =
        [&](TArray<TSoftObjectPtr<UWorld>>& Levels,
            bool bServerOnly)
        {
            if (!IsSaneArray(Levels.Num(), Levels.Max(), 64))
            {
                bInvalidLevelArray = true;
                return;
            }

            DeclaredLevels += Levels.Num();
            for (int32 Index = 0; Index < Levels.Num(); ++Index)
            {
                auto& Level =
                    Levels.Get(Index, FSoftObjectPtr::Size());
                const FName LevelName =
                    Level.ObjectID.AssetPathName;
                if (!LevelName.IsValid())
                {
                    bInvalidLevelArray = true;
                    continue;
                }
                ++ValidLevelEntries;
                if (HasStreamedPlaylistLevel(GameState, LevelName))
                    continue;

                ++RequestedLevels;
                bool bSuccess = false;
                ULevelStreamingDynamic::
                    LoadLevelInstanceBySoftObjectPtr(
                        World, Level, FVector(), FRotator(),
                        &bSuccess, FString(), nullptr);
                if (!bSuccess)
                {
                    SDK::DbgLog(
                        "[Heist] failed to request playlist level %s\n",
                        LevelName.ToString().c_str());
                    continue;
                }

                const bool bAdded =
                    AddStreamedPlaylistLevel(
                    GameState, LevelName, bServerOnly);
                if (!bAdded &&
                    !HasStreamedPlaylistLevel(
                        GameState, LevelName))
                {
                    SDK::DbgLog(
                        "[Heist] playlist level loaded but could not "
                        "be recorded: %s\n",
                        LevelName.ToString().c_str());
                    continue;
                }

                ++LoadedLevels;
                if (bAdded)
                {
                    CallReflectedNoParams(
                        GameState,
                        "OnFinishedStreamingAdditionalPlaylistLevel");
                }
            }
        };

    if (Playlist->HasAdditionalLevels())
        LoadLevels(Playlist->AdditionalLevels, false);
    if (Playlist->HasAdditionalLevelsServerOnly())
        LoadLevels(Playlist->AdditionalLevelsServerOnly, true);

    if (DeclaredLevels == 0 || ValidLevelEntries == 0)
    {
        if (!GHeistCompatibilityState
                 .bLoggedMissingAdditionalLevelData)
        {
            GHeistCompatibilityState
                .bLoggedMissingAdditionalLevelData = true;
            SDK::DbgLog(
                "[Heist] additional playlist level data is not "
                "available yet (declared=%d valid=%d)\n",
                DeclaredLevels, ValidLevelEntries);
        }
        return false;
    }

    if (LoadedLevels > 0)
        GameState->OnRep_AdditionalPlaylistLevelsStreamed();

    SDK::DbgLog(
        "[Heist] additional playlist levels requested=%d accepted=%d "
        "invalidArray=%d\n",
        RequestedLevels, LoadedLevels,
        bInvalidLevelArray ? 1 : 0);

    GHeistCompatibilityState.bAdditionalLevelsComplete =
        !bInvalidLevelArray &&
        ValidLevelEntries > 0 &&
        LoadedLevels == RequestedLevels;
    return GHeistCompatibilityState.bAdditionalLevelsComplete;
}

void FFortAthenaHeistCompatibility::Tick(
    UNetDriver* Driver,
    float DeltaSeconds)
{
    (void)DeltaSeconds;
    if (!IsSupportedBuild() || !Driver)
        return;

    UWorld* World = UWorld::GetWorld();
    if (!World || Driver != World->NetDriver ||
        !World->GameState)
    {
        return;
    }

    auto GameState =
        World->GameState->Cast<AFortGameStateAthena>();
    if (!GameState)
        return;

    UFortPlaylistAthena* Playlist = nullptr;
    if (GameState->HasCurrentPlaylistInfo() &&
        FPlaylistPropertyArray::HasBasePlaylist())
    {
        Playlist = const_cast<UFortPlaylistAthena*>(
            GameState->CurrentPlaylistInfo.BasePlaylist);
    }
    if (!Playlist && GameState->HasCurrentPlaylistData())
    {
        Playlist = const_cast<UFortPlaylistAthena*>(
            GameState->CurrentPlaylistData);
    }
    if (!IsHeistPlaylist(Playlist))
        return;

    if (GHeistCompatibilityState.World != World ||
        GHeistCompatibilityState.Playlist != Playlist)
    {
        ResetHeistCompatibilityState(World, Playlist);
    }

    const double Now =
        UGameplayStatics::GetTimeSeconds(World);
    if (!GHeistCompatibilityState.bPlaylistPrepared &&
        Now >= GHeistCompatibilityState
                   .NextPlaylistPreparationAttemptTime)
    {
        GHeistCompatibilityState.NextPlaylistPreparationAttemptTime =
            Now + 1.0;
        PreparePlaylist(GameState, Playlist);
    }
    if (!GHeistCompatibilityState.bAdditionalLevelsComplete &&
        Now >= GHeistCompatibilityState
                   .NextAdditionalLevelAttemptTime)
    {
        GHeistCompatibilityState.NextAdditionalLevelAttemptTime =
            Now + 1.0;
        LoadAdditionalPlaylistLevels(GameState, Playlist);
    }

    UpdateAndNotifyHeistGamePhaseStep(
        GameState, static_cast<float>(Now));
    TickExitCraftSpawners(GameState, Now);
}

namespace
{
    constexpr float DeepFriedObjectiveDistance = 6144.0f;
    constexpr float DeepFriedObjectiveZOffset = 15.0f;
    constexpr float DeepFriedObjectiveTraceHeight = 6000.0f;
    constexpr float NormalFoodFightTerrainTraceStartZ = 50000.0f;
    constexpr int32 NormalFoodFightSupportLiftCells = 3;
    constexpr float DeepFriedLegacyMapCenterX = 32000.0f;
    constexpr float DeepFriedLegacyMapCenterY = -25744.0f;
    constexpr uint8 DeepFriedBurgerTeam = 3;
    constexpr uint8 DeepFriedTomatoTeam = 4;
    constexpr uint8 DeepFriedBarrierStateDown = 2;
    constexpr uint8 DeepFriedObjectiveDamageStateMax = 9;

    bool IsFiniteArenaVector(const FVector& Value)
    {
        return std::isfinite(Value.X) &&
            std::isfinite(Value.Y) &&
            std::isfinite(Value.Z) &&
            std::fabs(Value.X) < 1000000.0 &&
            std::fabs(Value.Y) < 1000000.0 &&
            std::fabs(Value.Z) < 1000000.0;
    }

    bool IsUsableArenaCenter(const FVector& Value)
    {
        return IsFiniteArenaVector(Value) &&
            Value.X * Value.X + Value.Y * Value.Y >
                1000.0 * 1000.0;
    }

    bool TryCallSafeZoneCenter(
        const TScriptInterface<IInterface>& SafeZoneInterface,
        const char* FunctionName,
        FVector& OutCenter)
    {
        auto Object = const_cast<UObject*>(
            SafeZoneInterface.ObjectPointer);
        if (!IsLiveObject(Object) || !FunctionName)
            return false;

        auto Function = Object->GetFunction(FunctionName);
        if (!Function)
            return false;

        const FVector Candidate =
            Object->Call<FVector>(Function);
        if (!IsUsableArenaCenter(Candidate))
            return false;

        OutCenter = FVector(
            Candidate.X,
            Candidate.Y,
            Candidate.Z);
        return true;
    }

    bool TryReadGameModeSafeZoneCenter(
        UWorld* World,
        int32 PreferredIndex,
        FVector& OutCenter)
    {
        auto GameMode =
            World && IsLiveObject(World->AuthorityGameMode)
                ? World->AuthorityGameMode->Cast<AFortGameMode>()
                : nullptr;
        if (!IsLiveObject(GameMode) ||
            !GameMode->HasSafeZoneLocations())
        {
            return false;
        }

        auto& Locations = GameMode->SafeZoneLocations;
        const int32 VectorSize = FVector::Size();
        if (!IsSaneArray(
                Locations.Num(), Locations.Max(), 128) ||
            !IsReadableArrayStorage(
                Locations.GetData(),
                Locations.Num(),
                static_cast<size_t>(VectorSize)))
        {
            return false;
        }

        if (!Locations.IsValidIndex(PreferredIndex))
            return false;

        const auto& Candidate =
            Locations.Get(PreferredIndex, VectorSize);
        if (!IsUsableArenaCenter(Candidate))
            return false;

        OutCenter = FVector(
            Candidate.X,
            Candidate.Y,
            Candidate.Z);
        return true;
    }

    bool TryReadSafeZoneCenter(
        UWorld* World,
        const TScriptInterface<IInterface>& SafeZoneInterface,
        FVector& OutCenter,
        const char*& OutSource)
    {
        // At BusLocked, the current circle is the full-map phase at index 0.
        // The first visible white target is Next / SafeZoneLocations[1].
        // Never fall back to index 0 here: that is the island center, not the
        // Deep Fried objective circle.
        // The native location array is generated during playlist load, before
        // the indicator/interface publishes its phase preview. Prefer its
        // phase-1 entry so a stale indicator cannot win this timing race.
        if (TryReadGameModeSafeZoneCenter(
                World, 1, OutCenter))
        {
            OutSource = "SafeZoneLocations[1]";
            return true;
        }

        auto GameMode =
            World && IsLiveObject(World->AuthorityGameMode)
                ? World->AuthorityGameMode->Cast<AFortGameMode>()
                : nullptr;
        auto Indicator =
            IsLiveObject(GameMode) &&
                    GameMode->HasSafeZoneIndicator()
                ? GameMode->SafeZoneIndicator
                : nullptr;
        if (IsLiveObject(Indicator) &&
            Indicator->HasCurrentPhase() &&
            Indicator->CurrentPhase >= 1 &&
            Indicator->HasNextCenter() &&
            Indicator->HasNextRadius() &&
            std::isfinite(Indicator->NextRadius) &&
            Indicator->NextRadius > 0.0f &&
            IsUsableArenaCenter(Indicator->NextCenter))
        {
            OutCenter = Indicator->NextCenter;
            OutSource = "safe-zone indicator NextCenter";
            return true;
        }
        if (TryCallSafeZoneCenter(
                SafeZoneInterface,
                "GetSafeZoneNextCenter",
                OutCenter))
        {
            OutSource = "safe-zone Next center";
            return true;
        }
        return false;
    }

    bool ResolveDeepFriedDivider(
        AFortGameStateAthena* GameState,
        const TScriptInterface<IInterface>& SafeZoneInterface,
        FVector& OutDividerCenter,
        FVector& OutObjectiveCenter,
        bool& OutHasPlayableZoneCenter,
        float& OutYaw,
        const char*& OutSource)
    {
        if (!GameState)
            return false;

        OutHasPlayableZoneCenter = false;
        bool bHasFlightRotation = false;
        bool bHasFlightStart = false;
        FVector FlightStart;
        float FlightSpeed = 0.0f;
        float TimeTillFlightEnd = 0.0f;
        OutYaw = 0.0f;

        if (GameState->HasFlightPathMidLine() &&
            FAircraftFlightInfo::StaticStruct())
        {
            const auto& Flight = GameState->FlightPathMidLine;
            if (FAircraftFlightInfo::HasFlightStartRotation())
            {
                OutYaw = static_cast<float>(
                    Flight.FlightStartRotation.Yaw);
                bHasFlightRotation = std::isfinite(OutYaw);
            }
            if (FAircraftFlightInfo::HasFlightStartLocation())
            {
                const auto& Start = Flight.FlightStartLocation;
                FlightStart =
                    FVector(Start.X, Start.Y, Start.Z);
                bHasFlightStart =
                    IsUsableArenaCenter(FlightStart);
            }
            if (FAircraftFlightInfo::HasFlightSpeed())
                FlightSpeed = Flight.FlightSpeed;
            if (FAircraftFlightInfo::HasTimeTillFlightEnd())
                TimeTillFlightEnd = Flight.TimeTillFlightEnd;
        }

        if (!bHasFlightRotation)
            OutYaw = 0.0f;
        const double YawRadians =
            static_cast<double>(OutYaw) *
            3.14159265358979323846 / 180.0;
        const FVector DividerDirection(
            std::cos(YawRadians),
            std::sin(YawRadians),
            0.0);

        FVector SafeZoneCenter;
        if (TryReadSafeZoneCenter(
                UWorld::GetWorld(),
                SafeZoneInterface,
                SafeZoneCenter,
                OutSource))
        {
            OutHasPlayableZoneCenter = true;
            OutObjectiveCenter = SafeZoneCenter;
            OutDividerCenter = SafeZoneCenter;
            if (bHasFlightStart)
            {
                // Keep the actor on the authored FlightPathMidLine while
                // choosing the point on that line nearest the first zone.
                const FVector Delta =
                    SafeZoneCenter - FlightStart;
                const double AlongLine =
                    Delta.Dot(DividerDirection);
                OutDividerCenter =
                    FlightStart +
                    DividerDirection * AlongLine;
                OutDividerCenter.Z = SafeZoneCenter.Z;
            }
            return true;
        }

        if (bHasFlightStart &&
            std::isfinite(FlightSpeed) &&
            std::isfinite(TimeTillFlightEnd) &&
            FlightSpeed > 0.0f &&
            TimeTillFlightEnd > 0.0f)
        {
            OutDividerCenter =
                FlightStart +
                DividerDirection *
                    (static_cast<double>(FlightSpeed) *
                     static_cast<double>(TimeTillFlightEnd) *
                     0.5);
            OutDividerCenter.Z = 0.0;
            OutObjectiveCenter = OutDividerCenter;
            OutSource = "FlightPathMidLine midpoint";
            return IsUsableArenaCenter(OutDividerCenter);
        }

        // This is only an exact-10.40 final fallback.  The old Athena
        // gameplay origin is (0,0), while the island's calibrated center is
        // offset from it; never regress to placing the wall at world zero.
        OutDividerCenter = FVector(
            DeepFriedLegacyMapCenterX,
            DeepFriedLegacyMapCenterY,
            0.0f);
        OutObjectiveCenter = OutDividerCenter;
        OutSource = "10.40 island-center fallback";
        return true;
    }

    FVector FindDeepFriedGround(
        UWorld* World,
        AActor* IgnoreActor,
        const FVector& Candidate,
        float TraceHeight)
    {
        const FVector Result =
            UFortKismetLibrary::FindGroundLocationAt(
                World,
                IgnoreActor,
                Candidate,
                TraceHeight,
                -10000.0f,
                FName());
        return IsFiniteArenaVector(Result)
            ? FVector(Result.X, Result.Y, Result.Z)
            : FVector(Candidate.X, Candidate.Y, 0.0f);
    }

    float QuantizeDeepFriedCardinalYaw(float Yaw)
    {
        return static_cast<float>(
            std::round(
                static_cast<double>(Yaw) / 90.0) *
            90.0);
    }

    FVector RotateDeepFriedYaw(
        const FVector& Value,
        float Yaw)
    {
        const double Radians =
            static_cast<double>(Yaw) *
            3.14159265358979323846 / 180.0;
        const double CosYaw = std::cos(Radians);
        const double SinYaw = std::sin(Radians);
        return FVector(
            Value.X * CosYaw - Value.Y * SinYaw,
            Value.X * SinYaw + Value.Y * CosYaw,
            Value.Z);
    }

    struct FDeepFriedStructuralGrid
    {
        UBuildingStructuralSupportSystem* System = nullptr;
        FVector BaseLocToPivotOffset;
        FVector SnapOffset;
        FVector CentroidOffset;
        float HorizontalGridSize = 0.0f;
        float VerticalGridSize = 0.0f;
    };

    bool ResolveDeepFriedStructuralGrid(
        AFortGameStateAthena* GameState,
        const UClass* FlagClass,
        FDeepFriedStructuralGrid& OutGrid)
    {
        if (!IsLiveObject(GameState) ||
            !GameState->HasStructuralSupportSystem() ||
            !IsLiveObject(GameState->StructuralSupportSystem) ||
            !IsLiveObject(FlagClass))
        {
            return false;
        }

        auto FlagDefault =
            static_cast<const AAthenaBarrierFlag*>(
                FlagClass->GetDefaultObj());
        if (!IsLiveObject(
                const_cast<AAthenaBarrierFlag*>(FlagDefault)) ||
            !FlagDefault->HasSnapGridSize() ||
            !FlagDefault->HasVertSnapGridSize() ||
            !FlagDefault->HasBaseLocToPivotOffset() ||
            !FlagDefault->HasSnapOffset() ||
            !FlagDefault->HasCentroidOffset())
        {
            return false;
        }

        auto System =
            static_cast<UBuildingStructuralSupportSystem*>(
                GameState->StructuralSupportSystem);
        if (!System->GetFunction(
                "K2_GetGridIndicesFromWorldLoc") ||
            !System->GetFunction(
                "K2_GetWorldLocFromGridIndices"))
        {
            return false;
        }

        const float HorizontalGrid =
            FlagDefault->SnapGridSize;
        const float VerticalGrid =
            FlagDefault->VertSnapGridSize;
        const FVector BaseOffset =
            FlagDefault->BaseLocToPivotOffset;
        const FVector SnapOffset =
            FlagDefault->SnapOffset;
        const FVector CentroidOffset =
            FlagDefault->CentroidOffset;
        if (!std::isfinite(HorizontalGrid) ||
            HorizontalGrid <= 1.0f ||
            !std::isfinite(VerticalGrid) ||
            VerticalGrid <= 1.0f ||
            !IsFiniteArenaVector(BaseOffset) ||
            !IsFiniteArenaVector(SnapOffset) ||
            !IsFiniteArenaVector(CentroidOffset))
        {
            return false;
        }

        OutGrid.System = System;
        OutGrid.BaseLocToPivotOffset = FVector(
            BaseOffset.X, BaseOffset.Y, BaseOffset.Z);
        OutGrid.SnapOffset = FVector(
            SnapOffset.X, SnapOffset.Y, SnapOffset.Z);
        OutGrid.CentroidOffset = FVector(
            CentroidOffset.X,
            CentroidOffset.Y,
            CentroidOffset.Z);
        OutGrid.HorizontalGridSize = HorizontalGrid;
        OutGrid.VerticalGridSize = VerticalGrid;
        return true;
    }

    bool ResolveDeepFriedLavaSupportZ(
        float& OutFinalLavaZ,
        float& OutBuildableOffset,
        float& OutSupportZ)
    {
        auto FillClass =
            const_cast<UClass*>(
                AFortAthenaMutator_Fill::StaticClass());
        auto Fill =
            static_cast<AFortAthenaMutator_Fill*>(
                FindWorldMutator(FillClass));
        if (!IsLiveObject(Fill) ||
            !Fill->HasLavaFloor() ||
            !IsLiveObject(Fill->LavaFloor) ||
            !Fill->HasBuildableOffset())
        {
            return false;
        }

        auto LavaFloor = Fill->LavaFloor;
        float FinalLavaZ =
            -std::numeric_limits<float>::infinity();
        if (LavaFloor->HasFloorZ() &&
            std::isfinite(LavaFloor->FloorZ))
        {
            FinalLavaZ = LavaFloor->FloorZ;
        }

        if (!FFillFloorPositionData::StaticStruct() ||
            !FFillFloorPositionData::HasHeight())
        {
            return false;
        }

        const int32 RowSize =
            FFillFloorPositionData::Size();
        if (RowSize <= 0 || RowSize > 0x200)
            return false;

        bool bFoundAuthoredHeight = false;
        auto AccumulateFloorRows =
            [&](AAthenaFillFloor* Source)
        {
            if (!IsLiveObject(Source) ||
                !Source->HasFloorPositionData())
            {
                return;
            }

            auto& Rows = Source->FloorPositionData;
            if (!IsSaneArray(
                    Rows.Num(), Rows.Max(), 64) ||
                !IsReadableArrayStorage(
                    Rows.GetData(),
                    Rows.Num(),
                    static_cast<size_t>(RowSize)))
            {
                return;
            }

            for (int32 RowIndex = 0;
                 RowIndex < Rows.Num();
                 ++RowIndex)
            {
                auto& Row = Rows.Get(RowIndex, RowSize);
                const float Height =
                    Row.Height.Evaluate();
                if (!std::isfinite(Height))
                    continue;
                FinalLavaZ =
                    (std::max)(FinalLavaZ, Height);
                bFoundAuthoredHeight = true;
            }
        };
        AccumulateFloorRows(LavaFloor);
        if (!bFoundAuthoredHeight &&
            IsLiveObject(LavaFloor->Class))
        {
            // Construction can briefly expose the live floor before its
            // instanced array copy is complete.  The spawned class CDO owns
            // the same authored final-height rows and is safe to consult
            // during that narrow BusLocked timing window.
            AccumulateFloorRows(
                static_cast<AAthenaFillFloor*>(
                    const_cast<UObject*>(
                        LavaFloor->Class->GetDefaultObj())));
        }

        const float BuildableOffset =
            Fill->BuildableOffset.Evaluate();
        if (!bFoundAuthoredHeight ||
            !std::isfinite(FinalLavaZ) ||
            !std::isfinite(BuildableOffset) ||
            BuildableOffset < 0.0f)
        {
            return false;
        }

        const float SupportZ =
            FinalLavaZ + BuildableOffset;
        if (!std::isfinite(SupportZ))
            return false;

        OutFinalLavaZ = FinalLavaZ;
        OutBuildableOffset = BuildableOffset;
        OutSupportZ = SupportZ;
        return true;
    }

    bool DeepFriedWorldToGrid(
        const FDeepFriedStructuralGrid& Grid,
        const FVector& WorldLocation,
        FBuildingSupportCellIndex& OutIndex)
    {
        if (!Grid.System ||
            !IsFiniteArenaVector(WorldLocation))
        {
            return false;
        }

        FVector Input(
            WorldLocation.X,
            WorldLocation.Y,
            WorldLocation.Z);
        FBuildingSupportCellIndex Candidate{};
        if (!Grid.System->K2_GetGridIndicesFromWorldLoc(
                Input, &Candidate))
        {
            return false;
        }
        OutIndex = Candidate;
        return true;
    }

    bool DeepFriedGridToWorld(
        const FDeepFriedStructuralGrid& Grid,
        const FBuildingSupportCellIndex& Index,
        FVector& OutWorldLocation)
    {
        if (!Grid.System)
            return false;

        FBuildingSupportCellIndex Input = Index;
        FVector Candidate{};
        if (!Grid.System->K2_GetWorldLocFromGridIndices(
                Input, &Candidate) ||
            !IsFiniteArenaVector(Candidate))
        {
            return false;
        }
        OutWorldLocation = Candidate;
        return true;
    }

    struct FDeepFriedObjectivePlacement
    {
        FVector PairCenter;
        FVector Locations[2];
        FVector Grounds[2];
        float GroundDifference = 0.0f;
        bool bValid = false;
    };

    FDeepFriedObjectivePlacement
        FindDeepFriedObjectivePlacement(
            UWorld* World,
            AFortAthenaMutator_Barrier* Barrier,
            const FDeepFriedStructuralGrid& Grid,
            const FVector& PairAnchor,
            float DividerYaw,
            float Distance,
            float TraceHeight,
            float PrefabYaw,
            float ZOffset,
            float LavaSupportZ,
            float MaxGroundDifference,
            bool bSameHeight)
    {
        const double YawRadians =
            static_cast<double>(DividerYaw) *
            3.14159265358979323846 / 180.0;
        const FVector WallNormal(
            -std::sin(YawRadians),
            std::cos(YawRadians),
            0.0);

        FDeepFriedObjectivePlacement Placement{};
        const FVector DesiredRoots[2] = {
            PairAnchor + WallNormal * Distance,
            PairAnchor - WallNormal * Distance
        };
        Placement.Grounds[0] =
            FindDeepFriedGround(
                World,
                Barrier,
                DesiredRoots[0],
                TraceHeight);
        Placement.Grounds[1] =
            FindDeepFriedGround(
                World,
                Barrier,
                DesiredRoots[1],
                TraceHeight);

        FVector RotatedBaseOffsets[2] = {
            RotateDeepFriedYaw(
                Grid.BaseLocToPivotOffset,
                PrefabYaw),
            RotateDeepFriedYaw(
                Grid.BaseLocToPivotOffset,
                PrefabYaw + 180.0f)
        };
        FBuildingSupportCellIndex InitialIndices[2]{};
        double DesiredBaseZs[2]{};
        for (int32 TeamIndex = 0;
             TeamIndex < 2;
             ++TeamIndex)
        {
            const double DesiredBaseZ = (std::max)(
                static_cast<double>(
                    Placement.Grounds[TeamIndex].Z +
                    ZOffset),
                static_cast<double>(LavaSupportZ));
            DesiredBaseZs[TeamIndex] = DesiredBaseZ;
            FVector DesiredBase =
                DesiredRoots[TeamIndex] -
                RotatedBaseOffsets[TeamIndex];
            DesiredBase.Z = DesiredBaseZ;
            if (!DeepFriedWorldToGrid(
                    Grid,
                    DesiredBase,
                    InitialIndices[TeamIndex]))
            {
                return Placement;
            }
        }

        FVector InitialBaseLocations[2]{};
        for (int32 TeamIndex = 0;
             TeamIndex < 2;
             ++TeamIndex)
        {
            if (!DeepFriedGridToWorld(
                    Grid,
                    InitialIndices[TeamIndex],
                    InitialBaseLocations[TeamIndex]))
            {
                return Placement;
            }
            int32 Guard = 0;
            while (InitialBaseLocations[TeamIndex].Z + 0.01 <
                       DesiredBaseZs[TeamIndex] &&
                   Guard++ < 16)
            {
                ++InitialIndices[TeamIndex].Z;
                if (!DeepFriedGridToWorld(
                        Grid,
                        InitialIndices[TeamIndex],
                        InitialBaseLocations[TeamIndex]))
                {
                    return Placement;
                }
            }
            if (InitialBaseLocations[TeamIndex].Z + 0.01 <
                DesiredBaseZs[TeamIndex])
            {
                return Placement;
            }
        }

        if (bSameHeight)
        {
            const int32 CommonZ = (std::max)(
                InitialIndices[0].Z,
                InitialIndices[1].Z);
            InitialIndices[0].Z = CommonZ;
            InitialIndices[1].Z = CommonZ;
            int32 Guard = 0;
            while (Guard++ < 16)
            {
                bool bBothHighEnough = true;
                for (int32 TeamIndex = 0;
                     TeamIndex < 2;
                     ++TeamIndex)
                {
                    if (!DeepFriedGridToWorld(
                            Grid,
                            InitialIndices[TeamIndex],
                            InitialBaseLocations[TeamIndex]))
                    {
                        return Placement;
                    }
                    if (InitialBaseLocations[TeamIndex].Z +
                            0.01 <
                        DesiredBaseZs[TeamIndex])
                    {
                        bBothHighEnough = false;
                    }
                }
                if (bBothHighEnough)
                    break;
                ++InitialIndices[0].Z;
                ++InitialIndices[1].Z;
            }
            if (InitialBaseLocations[0].Z + 0.01 <
                    DesiredBaseZs[0] ||
                InitialBaseLocations[1].Z + 0.01 <
                    DesiredBaseZs[1])
            {
                return Placement;
            }
        }

        struct FCandidate
        {
            FBuildingSupportCellIndex Index;
            FVector Root;
            FVector Ground;
        };
        FCandidate Candidates[2][25]{};
        int32 CandidateCounts[2]{};
        for (int32 TeamIndex = 0;
             TeamIndex < 2;
             ++TeamIndex)
        {
            for (int32 DeltaX = -2;
                 DeltaX <= 2;
                 ++DeltaX)
            {
                for (int32 DeltaY = -2;
                     DeltaY <= 2;
                     ++DeltaY)
                {
                    auto Index =
                        InitialIndices[TeamIndex];
                    Index.X += DeltaX;
                    Index.Y += DeltaY;
                    FVector BaseLocation;
                    if (!DeepFriedGridToWorld(
                            Grid,
                            Index,
                            BaseLocation))
                    {
                        continue;
                    }

                    FVector Root =
                        BaseLocation +
                        RotatedBaseOffsets[TeamIndex];
                    FVector Ground =
                        FindDeepFriedGround(
                            World,
                            Barrier,
                            Root,
                            TraceHeight);
                    const double DesiredBaseZ = (std::max)(
                        static_cast<double>(
                            Ground.Z + ZOffset),
                        static_cast<double>(LavaSupportZ));
                    int32 Guard = 0;
                    while (BaseLocation.Z + 0.01 <
                               DesiredBaseZ &&
                           Guard++ < 16)
                    {
                        ++Index.Z;
                        if (!DeepFriedGridToWorld(
                                Grid,
                                Index,
                                BaseLocation))
                        {
                            break;
                        }
                    }
                    if (BaseLocation.Z + 0.01 <
                            DesiredBaseZ ||
                        !IsFiniteArenaVector(Ground))
                    {
                        continue;
                    }

                    Root =
                        BaseLocation +
                        RotatedBaseOffsets[TeamIndex];
                    auto& Candidate =
                        Candidates[TeamIndex]
                                  [CandidateCounts[TeamIndex]++];
                    Candidate.Index = Index;
                    Candidate.Root = Root;
                    Candidate.Ground = Ground;
                }
            }
            if (CandidateCounts[TeamIndex] == 0)
                return Placement;
        }

        double BestMidpointError =
            std::numeric_limits<double>::infinity();
        double BestSecondaryError =
            std::numeric_limits<double>::infinity();
        for (int32 FirstIndex = 0;
             FirstIndex < CandidateCounts[0];
             ++FirstIndex)
        {
            for (int32 SecondIndex = 0;
                 SecondIndex < CandidateCounts[1];
                 ++SecondIndex)
            {
                FVector Root0 =
                    Candidates[0][FirstIndex].Root;
                FVector Root1 =
                    Candidates[1][SecondIndex].Root;
                FVector Ground0 =
                    Candidates[0][FirstIndex].Ground;
                FVector Ground1 =
                    Candidates[1][SecondIndex].Ground;
                const double CandidateGroundDifference =
                    std::fabs(Ground0.Z - Ground1.Z);
                if (std::isfinite(MaxGroundDifference) &&
                    MaxGroundDifference >= 0.0f &&
                    CandidateGroundDifference >
                        static_cast<double>(
                            MaxGroundDifference) +
                            0.01)
                {
                    continue;
                }

                if (bSameHeight)
                {
                    const double CommonRootZ = (std::max)(
                        Root0.Z, Root1.Z);
                    Root0.Z = CommonRootZ;
                    Root1.Z = CommonRootZ;
                }
                const FVector Midpoint =
                    (Root0 + Root1) * 0.5;
                const FVector MidpointDelta =
                    Midpoint - PairAnchor;
                const FVector DesiredDelta0 =
                    Root0 - DesiredRoots[0];
                const FVector DesiredDelta1 =
                    Root1 - DesiredRoots[1];
                const FVector SeparationDelta =
                    Root0 - Root1;
                const double Separation = std::sqrt(
                    SeparationDelta.X * SeparationDelta.X +
                    SeparationDelta.Y * SeparationDelta.Y);
                const double SeparationError =
                    Separation -
                    static_cast<double>(Distance) * 2.0;
                // Preserve the zone midpoint first, then choose the nearest
                // symmetric authored cells.  Searching as a pair prevents
                // independent half-cell rounding from walking both bases
                // away from the visible circle.
                const double MidpointError =
                    (MidpointDelta.X * MidpointDelta.X +
                     MidpointDelta.Y * MidpointDelta.Y);
                const double SecondaryError =
                    (DesiredDelta0.X * DesiredDelta0.X +
                     DesiredDelta0.Y * DesiredDelta0.Y +
                     DesiredDelta1.X * DesiredDelta1.X +
                     DesiredDelta1.Y * DesiredDelta1.Y) +
                    SeparationError * SeparationError * 4.0;
                const bool bCloserMidpoint =
                    MidpointError + 0.01 <
                    BestMidpointError;
                const bool bSameMidpoint =
                    std::fabs(
                        MidpointError -
                        BestMidpointError) <= 0.01;
                if (!bCloserMidpoint &&
                    (!bSameMidpoint ||
                     SecondaryError >=
                         BestSecondaryError))
                {
                    continue;
                }

                BestMidpointError = MidpointError;
                BestSecondaryError = SecondaryError;
                Placement.Locations[0] = FVector(
                    Root0.X, Root0.Y, Root0.Z);
                Placement.Locations[1] = FVector(
                    Root1.X, Root1.Y, Root1.Z);
                Placement.PairCenter = FVector(
                    Midpoint.X, Midpoint.Y, Midpoint.Z);
                Placement.Grounds[0] = Ground0;
                Placement.Grounds[1] = Ground1;
            }
        }

        const double GroundDifference = std::fabs(
            Placement.Grounds[0].Z -
            Placement.Grounds[1].Z);
        Placement.GroundDifference =
            std::isfinite(GroundDifference)
                ? static_cast<float>(GroundDifference)
                : 0.0f;
        // Deep Fried forces both floating bases onto one lava-safe floor
        // plane below. Normal Food Fight additionally rejects candidate
        // pairs whose terrain exceeds its authored ObjectiveMaxZDiff.
        Placement.bValid =
            IsFiniteArenaVector(Placement.Locations[0]) &&
            IsFiniteArenaVector(Placement.Locations[1]) &&
            IsFiniteArenaVector(Placement.Grounds[0]) &&
            IsFiniteArenaVector(Placement.Grounds[1]) &&
            std::isfinite(BestMidpointError) &&
            std::isfinite(BestSecondaryError);
        return Placement;
    }

    void SetDeepFriedActorTeam(
        AAthenaBarrierFlag* Flag,
        AAthenaBarrierObjective* Objective,
        uint8 Team)
    {
        if (IsLiveObject(Flag))
            Flag->SetTeam(Team);
        if (IsLiveObject(Objective))
            Objective->SetTeam(Team);
    }

    bool PublishDeepFriedObjectiveHud(
        AFortGameStateAthena* GameState,
        FDeepFriedArenaState& Arena)
    {
        if (!IsLiveObject(GameState) ||
            !GameState->HasMutatorObjectDataArray() ||
            !FGameplayMutatorObjectData::StaticStruct() ||
            !FGameplayMutatorObjectDataArray::StaticStruct() ||
            !FGameplayMutatorObjectData::HasTheObject() ||
            !FGameplayMutatorObjectData::HasObjectId() ||
            !FGameplayMutatorObjectData::HasObjectValue1() ||
            !FGameplayMutatorObjectData::HasObjectValue2() ||
            !FGameplayMutatorObjectDataArray::
                HasObjectDataList())
        {
            return false;
        }

        const int32 EntrySize =
            FGameplayMutatorObjectData::Size();
        if (EntrySize < 0x24 || EntrySize > 0x100)
            return false;

        auto& Serializer =
            GameState->MutatorObjectDataArray;
        auto& Entries = Serializer.ObjectDataList;
        if (!IsSaneArray(
                Entries.Num(), Entries.Max(), 256) ||
            !IsReadableArrayStorage(
                Entries.GetData(),
                Entries.Num(),
                static_cast<size_t>(EntrySize)))
        {
            return false;
        }

        bool bChanged = false;
        const bool bInitialPublication =
            !Arena.bHudPublished;
        const double Now =
            IsLiveObject(Arena.World)
                ? UGameplayStatics::GetTimeSeconds(
                      Arena.World)
                : 0.0;
        const bool bRefreshPublication =
            bInitialPublication ||
            Now >= Arena.NextHudRefreshTime;
        int32 PublishedEntries = 0;
        bool bTeamPublished[2]{};
        for (int32 TeamIndex = 0;
             TeamIndex < 2;
             ++TeamIndex)
        {
            auto Flag = Arena.Flags[TeamIndex];
            auto Objective = Arena.Objectives[TeamIndex];
            if (!IsLiveObject(Flag) ||
                !IsLiveObject(Objective))
                continue;

            int32 Team =
                TeamIndex == 0
                    ? DeepFriedBurgerTeam
                    : DeepFriedTomatoTeam;
            int32 DamageState =
                DeepFriedObjectiveDamageStateMax;
            if (Objective->HasObjectiveDamageState())
            {
                DamageState =
                    static_cast<int32>(
                        Objective->ObjectiveDamageState);
                if (DamageState < 0 ||
                    DamageState >
                        DeepFriedObjectiveDamageStateMax)
                {
                    DamageState =
                        DeepFriedObjectiveDamageStateMax;
                }
            }

            FGameplayMutatorObjectData* StoredEntry =
                nullptr;
            for (int32 EntryIndex = 0;
                 EntryIndex < Entries.Num();
                 ++EntryIndex)
            {
                auto& Existing =
                    Entries.Get(EntryIndex, EntrySize);
                // BarrierWidgetBase consumes the replicated flag, then calls
                // GetObjectiveActor() to resolve the mascot head.  Older
                // compatibility builds published the head directly; accept
                // that record here so it is repaired in place instead of
                // leaving a permanently ignored duplicate.
                if (Existing.TheObject == Flag ||
                    Existing.TheObject == Objective)
                {
                    StoredEntry = &Existing;
                    break;
                }
            }

            if (!StoredEntry)
            {
                auto NewEntry =
                    static_cast<
                        FGameplayMutatorObjectData*>(
                        FMemory::Malloc(EntrySize));
                if (!NewEntry)
                    continue;

                memset(NewEntry, 0, EntrySize);
                NewEntry->ReplicationID = -1;
                NewEntry->ReplicationKey = -1;
                NewEntry->MostRecentArrayReplicationKey =
                    -1;
                NewEntry->TheObject = Flag;
                NewEntry->ObjectId = Team;
                NewEntry->ObjectValue1 = TeamIndex;
                NewEntry->ObjectValue2 = DamageState;
                StoredEntry =
                    &Entries.Add(*NewEntry, EntrySize);
                FMemory::Free(NewEntry);
                Serializer.MarkItemDirty(*StoredEntry);
                bChanged = true;
            }
            else
            {
                bool bEntryChanged = false;
                if (StoredEntry->TheObject != Flag)
                {
                    StoredEntry->TheObject = Flag;
                    bEntryChanged = true;
                }
                if (StoredEntry->ObjectId != Team)
                {
                    StoredEntry->ObjectId = Team;
                    bEntryChanged = true;
                }
                if (StoredEntry->ObjectValue1 != TeamIndex)
                {
                    StoredEntry->ObjectValue1 =
                        TeamIndex;
                    bEntryChanged = true;
                }
                if (StoredEntry->ObjectValue2 !=
                    DamageState)
                {
                    StoredEntry->ObjectValue2 =
                        DamageState;
                    bEntryChanged = true;
                }
                if (bEntryChanged)
                {
                    Serializer.MarkItemDirty(
                        *StoredEntry);
                    bChanged = true;
                }
            }
            // BarrierWidgetBase subscribes to fast-array item callbacks rather
            // than polling the array. Re-announce the unchanged pair at a low
            // frequency so a widget created/rebuilt after the first snapshot
            // still receives both objective-flag records.
            if (StoredEntry && bRefreshPublication)
                Serializer.MarkItemDirty(*StoredEntry);
            if (StoredEntry)
            {
                ++PublishedEntries;
                bTeamPublished[TeamIndex] = true;
                Flag->ForceNetUpdate();
                Objective->ForceNetUpdate();
            }
            Arena.LastPublishedDamageState[TeamIndex] =
                static_cast<uint8>(DamageState);
        }

        const bool bBothTeamsSettled =
            (bTeamPublished[0] ||
             Arena.bObjectiveDestroyed[0]) &&
            (bTeamPublished[1] ||
             Arena.bObjectiveDestroyed[1]);
        const bool bPublicationComplete =
            PublishedEntries == 2 ||
            (Arena.bHudPublished && bBothTeamsSettled);
        if (bChanged ||
            (bRefreshPublication && PublishedEntries > 0))
        {
            // MarkItemDirty advances each item key. The explicit array
            // publication makes the two objective records one complete
            // snapshot for BarrierWidgetBase.
            Serializer.MarkArrayDirty();
            GameState->ForceNetUpdate();
        }
        if (bPublicationComplete)
        {
            if (bRefreshPublication)
                Arena.NextHudRefreshTime = Now + 2.0;
            if (PublishedEntries == 2 &&
                !Arena.bHudPublished)
            {
                Arena.bHudPublished = true;
                SDK::DbgLog(
                    "[FoodFight] published objective HUD state: "
                    "entries=%d object=BarrierFlag "
                    "burger=(team=3 food=0) "
                    "tomato=(team=4 food=1)\n",
                    PublishedEntries);
            }
        }
        return bPublicationComplete;
    }

    float ReadDeepFriedObjectiveHealth(
        AAthenaBarrierObjective* Objective)
    {
        if (!IsLiveObject(Objective) ||
            !Objective->GetFunction("GetHealth"))
        {
            return -1.0f;
        }

        const float Health = Objective->GetHealth();
        return std::isfinite(Health)
            ? Health
            : -1.0f;
    }

    float ReadDeepFriedObjectiveMaxHealth(
        AAthenaBarrierObjective* Objective)
    {
        if (!IsLiveObject(Objective) ||
            !Objective->GetFunction("GetMaxHealth"))
        {
            return -1.0f;
        }

        const float MaxHealth = Objective->GetMaxHealth();
        return std::isfinite(MaxHealth) &&
                MaxHealth > 0.0f
            ? MaxHealth
            : -1.0f;
    }

    void UpdateDeepFriedObjectiveHealthVisual(
        AAthenaBarrierObjective* Objective,
        float Health)
    {
        if (!IsLiveObject(Objective) ||
            !Objective->GetFunction("UpdateInGameHealth"))
        {
            return;
        }

        const float MaxHealth =
            ReadDeepFriedObjectiveMaxHealth(Objective);
        if (MaxHealth <= 0.0f)
            return;

        const float HealthPercent =
            (std::max)(
                0.0f,
                (std::min)(Health / MaxHealth, 1.0f));
        Objective->UpdateInGameHealth(HealthPercent);
    }

    void FinishNormalFoodFightForObjectiveLoss(
        FDeepFriedArenaState& Arena,
        AFortGameStateAthena* GameState,
        int32 DestroyedTeamIndex)
    {
        if (!IsLiveObject(Arena.Barrier) ||
            !IsLiveObject(GameState) ||
            DestroyedTeamIndex < 0 ||
            DestroyedTeamIndex >= 2 ||
            IsNativeDeepFriedDescriptor(
                GNativeLTMCompatibilityState.Descriptor) ||
            !Arena.Barrier
                 ->HasbGameEndsWhenObjectiveIsDestroyed() ||
            !Arena.Barrier
                 ->bGameEndsWhenObjectiveIsDestroyed)
        {
            return;
        }

        auto World = UWorld::GetWorld();
        auto GameMode =
            World && IsLiveObject(World->AuthorityGameMode)
                ? World->AuthorityGameMode
                      ->Cast<AFortGameMode>()
                : nullptr;
        if (!IsLiveObject(GameMode))
            return;

        const bool bAlreadyTerminal =
            (GameState->HasGamePhase() &&
             GameState->GamePhase >=
                 static_cast<uint8>(
                     EAthenaGamePhase::EndGame)) ||
            (GameMode->HasMatchState() &&
             GameMode->MatchState ==
                 FName(L"WaitingPostMatch"));
        if (bAlreadyTerminal)
            return;

        const auto& WinningState =
            DestroyedTeamIndex == 0
                ? Arena.Barrier->Team_1_State
                : Arena.Barrier->Team_0_State;
        uint8 WinningTeam = WinningState.TeamNum;
        if (WinningTeam == 0 ||
            WinningTeam == 255)
        {
            WinningTeam =
                DestroyedTeamIndex == 0
                    ? DeepFriedTomatoTeam
                    : DeepFriedBurgerTeam;
        }

        AFortPlayerStateAthena* WinningPlayerState =
            nullptr;
        if (GameState->HasPlayerArray())
        {
            auto& Players = GameState->PlayerArray;
            if (IsSaneArray(
                    Players.Num(),
                    Players.Max(),
                    256) &&
                IsReadableArrayStorage(
                    Players.Data,
                    Players.Num(),
                    sizeof(AFortPlayerStateAthena*)))
            {
                for (auto PlayerState : Players)
                {
                    if (!IsLiveObject(PlayerState) ||
                        !PlayerState->HasTeamIndex() ||
                        PlayerState->TeamIndex !=
                            WinningTeam ||
                        (PlayerState->HasbIsSpectator() &&
                         PlayerState->bIsSpectator))
                    {
                        continue;
                    }
                    WinningPlayerState = PlayerState;
                    break;
                }
            }
        }

        if (GameState->HasWinningTeam())
        {
            GameState->WinningTeam = WinningTeam;
            GameState->OnRep_WinningTeam();
        }
        if (WinningPlayerState &&
            GameState->HasWinningPlayerState())
        {
            GameState->WinningPlayerState =
                WinningPlayerState;
            GameState->OnRep_WinningPlayerState();
        }
        if (WinningPlayerState &&
            WinningPlayerState->HasPlace())
        {
            WinningPlayerState->Place = 1;
            WinningPlayerState->OnRep_Place();
            WinningPlayerState->ForceNetUpdate();
        }
        GameState->ForceNetUpdate();

        if (auto EndMatch =
                GameMode->GetFunction("EndMatch"))
        {
            GameMode->Call<void>(EndMatch);
        }
        else
        {
            // EndMatch is reflected on stock 10.40. Keep a bounded fallback
            // so a stripped server cannot leave a completed objective match
            // accepting respawns indefinitely.
            if (GameState->HasGamePhase())
            {
                GameState->GamePhase =
                    static_cast<uint8>(
                        EAthenaGamePhase::EndGame);
                GameState->OnRep_GamePhase();
            }
            if (GameMode->HasMatchState())
                GameMode->MatchState =
                    FName(L"WaitingPostMatch");
            GameMode->ForceNetUpdate();
        }

        SDK::DbgLog(
            "[FoodFight] normal objective terminal fallback: "
            "destroyedTeam=%u winningTeam=%u winnerPS=%p\n",
            static_cast<unsigned>(
                DestroyedTeamIndex == 0
                    ? DeepFriedBurgerTeam
                    : DeepFriedTomatoTeam),
            static_cast<unsigned>(WinningTeam),
            static_cast<void*>(WinningPlayerState));
    }

    void HandleDeepFriedObjectiveDestroyed(
        FDeepFriedArenaState& Arena,
        int32 TeamIndex)
    {
        if (TeamIndex < 0 || TeamIndex >= 2 ||
            Arena.bObjectiveDestroyed[TeamIndex] ||
            !IsLiveObject(Arena.Barrier))
        {
            return;
        }

        auto Objective = Arena.Objectives[TeamIndex];
        auto Flag = Arena.Flags[TeamIndex];
        auto& TeamState =
            TeamIndex == 0
                ? Arena.Barrier->Team_0_State
                : Arena.Barrier->Team_1_State;
        const bool bNativeAlreadyDisabledRespawns =
            !TeamState.bRespawnEnabled;
        const bool bObjectiveIsBeingDestroyed =
            IsLiveObject(Objective) &&
            Objective->HasbActorIsBeingDestroyed() &&
            Objective->bActorIsBeingDestroyed;

        // Latch before the native terminal event so a re-entrant gameplay
        // callback cannot execute it twice.
        Arena.bObjectiveDestroyed[TeamIndex] = true;
        if (!bNativeAlreadyDisabledRespawns &&
            IsLiveObject(Objective) &&
            !bObjectiveIsBeingDestroyed &&
            Objective->GetFunction("OnGeneratorDestroyed"))
        {
            Objective->OnGeneratorDestroyed();
        }

        // The stock callback owns this transition.  The explicit write is a
        // compatibility fallback for manually spawned objectives whose
        // Blueprint delegate did not bind to the mutator early enough.
        TeamState.bRespawnEnabled = false;
        if (IsLiveObject(Objective))
        {
            if (Objective->HasObjectiveDamageState() &&
                Objective->ObjectiveDamageState ==
                    DeepFriedObjectiveDamageStateMax)
            {
                Objective->ObjectiveDamageState = 8;
                Objective->OnRep_ObjectiveDamageState();
            }
            Objective->ForceNetUpdate();
        }
        if (IsLiveObject(Flag))
        {
            if (Flag->HasCurrentState() &&
                Flag->CurrentState == 0)
            {
                Flag->CurrentState =
                    1; // EBarrierFlagState::FlagDown
                Flag->OnRep_CurrentState();
            }
            Flag->ForceNetUpdate();
        }
        Arena.Barrier->ForceNetUpdate();

        auto World = UWorld::GetWorld();
        auto GameState =
            World && IsLiveObject(World->GameState)
                ? World->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        if (IsLiveObject(GameState))
        {
            GameState->ForceNetUpdate();
            FinishNormalFoodFightForObjectiveLoss(
                Arena,
                GameState,
                TeamIndex);
        }

        SDK::DbgLog(
            "[FoodFight] %s objective destroyed; team=%u "
            "respawns disabled (nativeTransition=%d)\n",
            TeamIndex == 0 ? "burger" : "tomato",
            static_cast<unsigned>(
                TeamIndex == 0
                    ? DeepFriedBurgerTeam
                    : DeepFriedTomatoTeam),
            bNativeAlreadyDisabledRespawns ? 1 : 0);
    }

    int32 SendFoodFightGameMessage(
        AFortGameStateAthena* GameState,
        const FAthenaGameMessageData& Message,
        const char* MessageName)
    {
        if (!IsLiveObject(GameState) ||
            !FAthenaGameMessageData::StaticStruct() ||
            !FAthenaGameMessageData::HasMsgText() ||
            !GameState->HasPlayerArray())
        {
            return 0;
        }

        auto& Players = GameState->PlayerArray;
        if (!IsSaneArray(
                Players.Num(), Players.Max(), 256) ||
            !IsReadableArrayStorage(
                Players.Data,
                Players.Num(),
                sizeof(void*)))
        {
            return 0;
        }

        int32 RecipientCount = 0;
        for (auto PlayerState : Players)
        {
            if (!IsLiveObject(PlayerState) ||
                !PlayerState->HasOwner() ||
                !IsLiveObject(PlayerState->Owner))
            {
                continue;
            }
            if (FAthenaGameMessageData::HasbIsTeamBased() &&
                Message.bIsTeamBased &&
                FAthenaGameMessageData::HasTeamIndex() &&
                (!PlayerState->HasTeamIndex() ||
                 static_cast<int32>(PlayerState->TeamIndex) !=
                     Message.TeamIndex))
            {
                continue;
            }

            auto PlayerController =
                PlayerState->Owner
                    ->Cast<AFortPlayerControllerAthena>();
            if (!IsLiveObject(PlayerController))
                continue;
            UFunction* ClientSendMessage =
                PlayerController->GetFunction(
                    "ClientSendMessage");
            if (!ClientSendMessage)
                continue;

            UObject* StartSound =
                FAthenaGameMessageData::HasMsgSound()
                    ? Message.MsgSound
                    : nullptr;
            PlayerController->Call<void>(
                ClientSendMessage,
                Message.MsgText,
                StartSound);
            ++RecipientCount;
        }

        SDK::DbgLog(
            "[FoodFight] published authored %s notification "
            "to %d player controllers\n",
            MessageName ? MessageName : "wall",
            RecipientCount);
        return RecipientCount;
    }

    void RefreshDeepFriedArenaBindings()
    {
        UWorld* World = UWorld::GetWorld();
        auto& Arena = GDeepFriedArenaState;
        if (!World || Arena.World != World ||
            !IsLiveObject(Arena.Barrier))
        {
            return;
        }

        auto GameState =
            IsLiveObject(World->GameState)
                ? World->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        if (IsLiveObject(Arena.Wall) &&
            Arena.Wall->HasBarrierState())
        {
            const int32 WallState =
                static_cast<int32>(
                    Arena.Wall->BarrierState);
            if (WallState != Arena.LastObservedWallState)
            {
                SDK::DbgLog(
                    "[FoodFight] divider state %d -> %d\n",
                    Arena.LastObservedWallState,
                    WallState);
                Arena.LastObservedWallState = WallState;
            }

            if (WallState == 1 &&
                !Arena.bPublishedWallComingDown &&
                Arena.Barrier
                    ->HasGameMsg_WallComingDown())
            {
                Arena.bPublishedWallComingDown =
                    SendFoodFightGameMessage(
                        GameState,
                        Arena.Barrier
                            ->GameMsg_WallComingDown,
                        "wall-coming-down") > 0;
            }
            if (WallState ==
                    DeepFriedBarrierStateDown &&
                !Arena.bPublishedWallDown &&
                Arena.Barrier->HasGameMsg_WallDown())
            {
                Arena.bPublishedWallDown =
                    SendFoodFightGameMessage(
                        GameState,
                        Arena.Barrier->GameMsg_WallDown,
                        "wall-down") > 0;
            }
        }

        if (!IsLiveObject(Arena.Flags[0]) ||
            !IsLiveObject(Arena.Flags[1]))
        {
            return;
        }

        bool bWallDown =
            IsLiveObject(Arena.Wall) &&
            Arena.Wall->HasBarrierState() &&
            Arena.Wall->BarrierState ==
                DeepFriedBarrierStateDown;
        if (bWallDown != Arena.bDamageEnabled)
        {
            Arena.bDamageEnabled = bWallDown;
            SDK::DbgLog(
                "[FoodFight] objective damage %s "
                "(wallState=%u)\n",
                bWallDown ? "enabled" : "disabled",
                IsLiveObject(Arena.Wall) &&
                        Arena.Wall->HasBarrierState()
                    ? static_cast<unsigned>(
                          Arena.Wall->BarrierState)
                    : 255u);
        }

        for (int32 TeamIndex = 0;
             TeamIndex < 2;
             ++TeamIndex)
        {
            auto Flag = Arena.Flags[TeamIndex];
            const bool bLostBoundObjective =
                Arena.bBindingsComplete &&
                Arena.LastObjectiveHealth[TeamIndex] >=
                    0.0f &&
                !IsLiveObject(Arena.Objectives[TeamIndex]);
            const bool bFlagNoLongerUp =
                Arena.bBindingsComplete &&
                IsLiveObject(Flag) &&
                Flag->HasCurrentState() &&
                Flag->CurrentState != 0;
            if (!Arena.bObjectiveDestroyed[TeamIndex] &&
                (bFlagNoLongerUp ||
                 (Arena.bDamageEnabled &&
                  bLostBoundObjective)))
            {
                // Some legacy Blueprint variants destroy the mascot actor in
                // the terminal callback before the polling tick can observe
                // a literal zero. A lowered flag or the loss of a previously
                // healthy bound objective after the wall falls is the same
                // terminal state.
                HandleDeepFriedObjectiveDestroyed(
                    Arena, TeamIndex);
            }

            if (!Arena.bObjectiveDestroyed[TeamIndex] &&
                !IsLiveObject(Arena.Objectives[TeamIndex]))
            {
                Arena.Objectives[TeamIndex] =
                    Flag->GetObjectiveActor();
            }
        }
        if (!Arena.bBindingsComplete &&
            (!IsLiveObject(Arena.Objectives[0]) ||
             !IsLiveObject(Arena.Objectives[1])))
        {
            if (!Arena.bLoggedWaitingForObjectives)
            {
                Arena.bLoggedWaitingForObjectives = true;
                SDK::DbgLog(
                    "[FoodFight] floating objective child actors are "
                    "still registering; binding will retry next tick\n");
            }
            return;
        }

        auto Barrier = Arena.Barrier;
        if (!Arena.bBindingsComplete)
        {
            if (!FBarrierTeamState::StaticStruct() ||
                !Barrier->HasTeam_0_State() ||
                !Barrier->HasTeam_1_State() ||
                !FBarrierTeamState::HasTeamNum() ||
                !FBarrierTeamState::HasFoodTeam() ||
                !FBarrierTeamState::HasObjectiveFlag() ||
                !FBarrierTeamState::HasObjectiveObject() ||
                !FBarrierTeamState::HasbRespawnEnabled())
            {
                SDK::DbgLog(
                    "[FoodFight] exact 10.40 Barrier team-state "
                    "reflection was unavailable\n");
                return;
            }

            auto& Team0 = Barrier->Team_0_State;
            auto& Team1 = Barrier->Team_1_State;

            // Populate both complete state records before invoking callbacks:
            // those callbacks are allowed to query the opposing team's state.
            Team0.TeamNum =
                static_cast<uint8>(DeepFriedBurgerTeam);
            Team0.FoodTeam = 0;
            Team0.ObjectiveFlag = Arena.Flags[0];
            Team0.ObjectiveObject = Arena.Objectives[0];
            Team0.bRespawnEnabled = true;

            Team1.TeamNum =
                static_cast<uint8>(DeepFriedTomatoTeam);
            Team1.FoodTeam = 1;
            Team1.ObjectiveFlag = Arena.Flags[1];
            Team1.ObjectiveObject = Arena.Objectives[1];
            Team1.bRespawnEnabled = true;

            SetDeepFriedActorTeam(
                Arena.Flags[0],
                Arena.Objectives[0],
                DeepFriedBurgerTeam);
            SetDeepFriedActorTeam(
                Arena.Flags[1],
                Arena.Objectives[1],
                DeepFriedTomatoTeam);

            for (uint8 TeamIndex = 0;
                 TeamIndex < 2;
                 ++TeamIndex)
            {
                auto Flag = Arena.Flags[TeamIndex];
                auto Objective =
                    Arena.Objectives[TeamIndex];
                Flag->FoodTeam = TeamIndex;
                Objective->FoodTeam = TeamIndex;
                // MAX is the constructor's full-health sentinel. State 0
                // means the 75% threshold has already been crossed.
                Objective->ObjectiveDamageState =
                    static_cast<uint8>(
                        DeepFriedObjectiveDamageStateMax);
                Objective->bAllowDamage = false;
                Arena.LastObjectiveHealth[TeamIndex] =
                    ReadDeepFriedObjectiveHealth(
                        Objective);
                if (Arena.LastObjectiveHealth[TeamIndex] >=
                    0.0f)
                {
                    UpdateDeepFriedObjectiveHealthVisual(
                        Objective,
                        Arena.LastObjectiveHealth[TeamIndex]);
                }
            }
            for (uint8 TeamIndex = 0;
                 TeamIndex < 2;
                 ++TeamIndex)
            {
                auto Flag = Arena.Flags[TeamIndex];
                auto Objective =
                    Arena.Objectives[TeamIndex];
                Flag->OnRep_FoodTeam();
                Objective->OnRep_FoodTeam();
                Objective->OnRep_ObjectiveDamageState();
                Flag->CurrentState =
                    0; // EBarrierFlagState::FlagUp
                Flag->OnRep_CurrentState();
                Flag->ForceNetUpdate();
                Objective->ForceNetUpdate();
            }

            if (IsLiveObject(Arena.Wall))
                Arena.Wall->ForceNetUpdate();
            Barrier->ForceNetUpdate();
            Arena.bBindingsComplete = true;
            SDK::DbgLog(
                "[FoodFight] bound floating objectives: "
                "burgerFlag=%p burgerObjective=%p team=3 "
                "tomatoFlag=%p tomatoObjective=%p team=4\n",
                static_cast<void*>(Arena.Flags[0]),
                static_cast<void*>(Arena.Objectives[0]),
                static_cast<void*>(Arena.Flags[1]),
                static_cast<void*>(Arena.Objectives[1]));
        }

        UFunction* CheckHealthThresholdFunction =
            Barrier->GetFunction("CheckHealthThreshold");
        for (int32 TeamIndex = 0;
             TeamIndex < 2;
             ++TeamIndex)
        {
            auto Objective = Arena.Objectives[TeamIndex];
            if (!IsLiveObject(Objective))
                continue;

            if (Objective->HasbAllowDamage() &&
                Objective->bAllowDamage != bWallDown)
            {
                Objective->bAllowDamage = bWallDown;
                Objective->ForceNetUpdate();
            }

            const float Health =
                ReadDeepFriedObjectiveHealth(Objective);
            if (Health < 0.0f)
                continue;

            const float PreviousHealth =
                Arena.LastObjectiveHealth[TeamIndex];
            Arena.LastObjectiveHealth[TeamIndex] =
                Health;

            const bool bHealthChanged =
                PreviousHealth < 0.0f ||
                std::fabs(Health - PreviousHealth) >
                    0.01f;
            if (bHealthChanged)
            {
                UpdateDeepFriedObjectiveHealthVisual(
                    Objective, Health);
                if (bWallDown &&
                    CheckHealthThresholdFunction)
                {
                    Barrier->CheckHealthThreshold(
                        static_cast<uint8>(
                            TeamIndex == 0
                                ? DeepFriedBurgerTeam
                                : DeepFriedTomatoTeam));
                }
            }

            if (bWallDown && Health <= 0.01f)
            {
                HandleDeepFriedObjectiveDestroyed(
                    Arena, TeamIndex);
            }
        }

        if (!PublishDeepFriedObjectiveHud(
                GameState, Arena) &&
            !Arena.bLoggedHudUnavailable)
        {
            Arena.bLoggedHudUnavailable = true;
            SDK::DbgLog(
                "[FoodFight] objective HUD fast-array "
                "reflection is not available\n");
        }
    }

    AAthenaBarrierFlag* SpawnDeepFriedFlag(
        UWorld* World,
        AFortAthenaMutator_Barrier* Barrier,
        const UClass* FlagClass,
        const FVector& Location,
        float Yaw,
        uint8 FoodTeam)
    {
        if (!World || !IsLiveObject(Barrier) ||
            !IsLiveObject(FlagClass))
        {
            return nullptr;
        }

        auto Flag =
            World->SpawnActorUnfinished<AAthenaBarrierFlag>(
                FlagClass,
                Location,
                FRotator(0.0f, Yaw, 0.0f),
                Barrier);
        if (!IsLiveObject(Flag))
            return nullptr;

        // Seed the replicated discriminator before the prefab constructs its
        // mascot child, then run the canonical OnRep after both states exist.
        Flag->FoodTeam = FoodTeam;
        Flag->CurrentState = 0;
        Flag = World->FinishSpawnActor<AAthenaBarrierFlag>(
            Flag,
            Location,
            FRotator(0.0f, Yaw, 0.0f));
        return IsLiveObject(Flag) ? Flag : nullptr;
    }

    void EnsureDeepFriedArena(
        UWorld* World,
        AFortGameStateAthena* GameState,
        AFortAthenaMutator_Barrier* Barrier,
        const TScriptInterface<IInterface>& SafeZoneInterface)
    {
        if (!World || !GameState || !IsLiveObject(Barrier))
            return;

        auto& Arena = GDeepFriedArenaState;
        if (Arena.World != World)
        {
            Arena = {};
            Arena.World = World;
        }
        if (Arena.Barrier != Barrier)
        {
            Arena = {};
            Arena.World = World;
            Arena.Barrier = Barrier;
        }
        if (IsLiveObject(
                SafeZoneInterface.ObjectPointer))
        {
            Arena.SafeZoneInterface =
                SafeZoneInterface;
        }

        if (Barrier->HasBigBaseWall() &&
            IsLiveObject(Barrier->BigBaseWall))
        {
            Arena.Wall = Barrier->BigBaseWall;
        }
        if (FBarrierTeamState::StaticStruct() &&
            FBarrierTeamState::HasObjectiveFlag())
        {
            if (Barrier->HasTeam_0_State() &&
                IsLiveObject(
                    Barrier->Team_0_State.ObjectiveFlag))
            {
                Arena.Flags[0] =
                    Barrier->Team_0_State.ObjectiveFlag;
            }
            if (Barrier->HasTeam_1_State() &&
                IsLiveObject(
                    Barrier->Team_1_State.ObjectiveFlag))
            {
                Arena.Flags[1] =
                    Barrier->Team_1_State.ObjectiveFlag;
            }
        }

        FVector DividerCenter;
        FVector ObjectiveZoneCenter;
        bool bHasPlayableZoneCenter = false;
        float DividerYaw = 0.0f;
        const char* DividerSource = "unresolved";
        ResolveDeepFriedDivider(
            GameState,
            SafeZoneInterface,
            DividerCenter,
            ObjectiveZoneCenter,
            bHasPlayableZoneCenter,
            DividerYaw,
            DividerSource);

        const FVector WallLocation(
            DividerCenter.X,
            DividerCenter.Y,
            -2000.0f);
        if (!IsLiveObject(Arena.Wall))
        {
            const UClass* WallClass = nullptr;
            if (Barrier->HasBigBaseWallClass())
                WallClass = Barrier->BigBaseWallClass.Get();
            if (!IsLiveObject(WallClass))
            {
                WallClass = FindObject<UClass>(
                    L"/Game/Athena/Playlists/Barrier/"
                    L"Barrier.Barrier_C");
            }
            if (IsLiveObject(WallClass))
            {
                FQuat WallRotation =
                    FRotator(0.0f, DividerYaw, 0.0f);
                FTransform WallTransform(
                    WallLocation,
                    WallRotation);
                Arena.Wall =
                    static_cast<AAthenaBigBaseWall*>(
                        World->SpawnActor(
                            WallClass,
                            WallTransform,
                            Barrier,
                            1 /* AlwaysSpawn */));
                if (IsLiveObject(Arena.Wall))
                {
                    SDK::DbgLog(
                        "[FoodFight] spawned authored divider wall=%p "
                        "center=(%.1f,%.1f,-2000.0) yaw=%.1f "
                        "source=%s\n",
                        static_cast<void*>(Arena.Wall),
                        static_cast<float>(DividerCenter.X),
                        static_cast<float>(DividerCenter.Y),
                        DividerYaw,
                        DividerSource);
                    Arena.LastWallLocation.X = WallLocation.X;
                    Arena.LastWallLocation.Y = WallLocation.Y;
                    Arena.LastWallLocation.Z = WallLocation.Z;
                    Arena.LastWallYaw = DividerYaw;
                    Arena.bHasWallTransform = true;
                }
            }
        }
        else
        {
            const FVector WallDelta =
                WallLocation - Arena.LastWallLocation;
            float YawDelta =
                std::fmod(
                    DividerYaw - Arena.LastWallYaw,
                    360.0f);
            if (YawDelta > 180.0f)
                YawDelta -= 360.0f;
            else if (YawDelta < -180.0f)
                YawDelta += 360.0f;

            // The warmup pass may initially use the calibrated island center.
            // As soon as FlightPathMidLine or the first playable safe zone is
            // published, move the same replicated wall to its authored divider
            // transform before the objective pair is created.
            if (!Arena.bHasWallTransform ||
                WallDelta.SizeSquared() > 1.0 ||
                std::fabs(YawDelta) > 0.1f)
            {
                const bool bAligned =
                    Arena.Wall->K2_TeleportTo(
                        WallLocation,
                        FRotator(0.0f, DividerYaw, 0.0f));
                if (bAligned)
                {
                    Arena.LastWallLocation.X = WallLocation.X;
                    Arena.LastWallLocation.Y = WallLocation.Y;
                    Arena.LastWallLocation.Z = WallLocation.Z;
                    Arena.LastWallYaw = DividerYaw;
                    Arena.bHasWallTransform = true;
                    Arena.Wall->ForceNetUpdate();
                    SDK::DbgLog(
                        "[FoodFight] aligned divider wall=%p "
                        "center=(%.1f,%.1f,-2000.0) yaw=%.1f "
                        "source=%s\n",
                        static_cast<void*>(Arena.Wall),
                        static_cast<float>(DividerCenter.X),
                        static_cast<float>(DividerCenter.Y),
                        DividerYaw,
                        DividerSource);
                }
            }
        }
        if (Barrier->HasBigBaseWall() &&
            IsLiveObject(Arena.Wall))
        {
            Barrier->BigBaseWall = Arena.Wall;
        }

        const UClass* FlagClass = nullptr;
        if (Barrier->HasObjectiveFlag())
            FlagClass = Barrier->ObjectiveFlag.Get();
        if (!IsLiveObject(FlagClass))
        {
            FlagClass = IsNativeDeepFriedDescriptor(
                            GNativeLTMCompatibilityState
                                .Descriptor)
                ? FindObject<UClass>(
                      L"/Game/Athena/Playlists/Barrier/"
                      L"TeamObjective_Food1_Floating."
                      L"TeamObjective_Food1_Floating_C")
                : FindObject<UClass>(
                      L"/Game/Athena/Playlists/Barrier/"
                      L"TeamObjective_Food1."
                      L"TeamObjective_Food1_C");
        }

        const bool bObjectivesMaySpawn =
            GameState->HasGamePhaseStep() &&
            GameState->GamePhaseStep >=
                static_cast<uint8>(
                    EAthenaGamePhaseStep::BusLocked);
        if (IsLiveObject(FlagClass) &&
            bHasPlayableZoneCenter &&
            bObjectivesMaySpawn &&
            (!IsLiveObject(Arena.Flags[0]) ||
             !IsLiveObject(Arena.Flags[1])))
        {
            FDeepFriedStructuralGrid StructuralGrid{};
            if (!ResolveDeepFriedStructuralGrid(
                    GameState,
                    FlagClass,
                    StructuralGrid))
            {
                if (!Arena.bLoggedWaitingForStructuralGrid)
                {
                    Arena.bLoggedWaitingForStructuralGrid = true;
                    SDK::DbgLog(
                        "[FoodFight] deferred floating bases until the "
                        "authored building structural grid is ready\n");
                }
                return;
            }
            Arena.bLoggedWaitingForStructuralGrid = false;

            float FinalLavaZ = 0.0f;
            float BuildableOffset = 0.0f;
            float MinimumSupportZ =
                -std::numeric_limits<float>::infinity();
            const bool bDeepFried =
                IsNativeDeepFriedDescriptor(
                    GNativeLTMCompatibilityState
                        .Descriptor);
            if (bDeepFried &&
                !ResolveDeepFriedLavaSupportZ(
                    FinalLavaZ,
                    BuildableOffset,
                    MinimumSupportZ))
            {
                if (!Arena.bLoggedWaitingForLava)
                {
                    Arena.bLoggedWaitingForLava = true;
                    SDK::DbgLog(
                        "[FoodFight] deferred floating bases until the "
                        "native fill floor publishes its final authored "
                        "lava height\n");
                }
                return;
            }
            Arena.bLoggedWaitingForLava = false;

            float Distance = DeepFriedObjectiveDistance;
            if (Barrier->HasObjectiveDistanceFromWall())
            {
                const float Evaluated =
                    Barrier->ObjectiveDistanceFromWall.Evaluate();
                if (std::isfinite(Evaluated) &&
                    Evaluated > 0.0f)
                {
                    Distance = Evaluated;
                }
            }
            float ZOffset = DeepFriedObjectiveZOffset;
            if (Barrier->HasObjectiveZOffset())
            {
                const float Evaluated =
                    Barrier->ObjectiveZOffset.Evaluate();
                if (std::isfinite(Evaluated))
                    ZOffset = Evaluated;
            }
            float SupportLiftZ = 0.0f;
            if (!bDeepFried)
            {
                // The normal TeamObjective_Food1 prefab has three authored
                // support stories below its platform pivot.  Authentic Food
                // Fight raises that pivot by three building-grid levels so
                // the orange legs support a tall base above the landscape
                // instead of extending below it.
                SupportLiftZ =
                    StructuralGrid.VerticalGridSize *
                    static_cast<float>(
                        NormalFoodFightSupportLiftCells);
                if (std::isfinite(SupportLiftZ))
                    ZOffset += SupportLiftZ;
                else
                    SupportLiftZ = 0.0f;
            }
            float TraceHeight = DeepFriedObjectiveTraceHeight;
            if (Barrier->HasObjectiveMaxSpawnHeight())
            {
                const float Evaluated =
                    Barrier->ObjectiveMaxSpawnHeight.Evaluate();
                if (std::isfinite(Evaluated) &&
                    Evaluated > 0.0f)
                {
                    TraceHeight = Evaluated;
                }
            }
            if (!bDeepFried)
            {
                // ObjectiveMaxSpawnHeight is an absolute world-space trace
                // start.  Season X terrain around high POIs such as Gotham
                // can sit above the authored legacy ceiling; starting below
                // that landscape finds its underside or the low water plane
                // and buries the large stilt prefab.  Trace normal Food Fight
                // from above every 10.40 landscape while preserving Deep
                // Fried's authored trace and lava support path unchanged.
                TraceHeight = (std::max)(
                    TraceHeight,
                    NormalFoodFightTerrainTraceStartZ);
            }
            float MaxGroundDifference =
                std::numeric_limits<float>::infinity();
            if (!bDeepFried &&
                Barrier->HasObjectiveMaxZDiff())
            {
                const float Evaluated =
                    Barrier->ObjectiveMaxZDiff.Evaluate();
                if (std::isfinite(Evaluated) &&
                    Evaluated >= 0.0f)
                {
                    MaxGroundDifference = Evaluated;
                }
            }

            const float PrefabYaw =
                QuantizeDeepFriedCardinalYaw(
                    DividerYaw - 90.0f);
            const bool bSameHeight =
                !Barrier->HasObjectivesSpawnSameHeight() ||
                Barrier->ObjectivesSpawnSameHeight
                        .Evaluate() !=
                    0.0f;
            // Standard Food Fight authors its bases symmetrically around the
            // divider. Deep Fried intentionally centers its floating pair in
            // the first white zone, whose center can be off the flight line.
            const FVector PlacementAnchor =
                bDeepFried
                    ? ObjectiveZoneCenter
                    : DividerCenter;
            auto Placement =
                FindDeepFriedObjectivePlacement(
                    World,
                    Barrier,
                    StructuralGrid,
                    PlacementAnchor,
                    DividerYaw,
                    Distance,
                    TraceHeight,
                    PrefabYaw,
                    ZOffset,
                    MinimumSupportZ,
                    MaxGroundDifference,
                    bSameHeight);
            if (!Placement.bValid)
            {
                if (!Arena.bLoggedPlacementFailure)
                {
                    Arena.bLoggedPlacementFailure = true;
                    SDK::DbgLog(
                        "[FoodFight] deferred floating bases: "
                        "zone-centered pair transform was invalid\n");
                }
            }
            else
            {
                Arena.bLoggedPlacementFailure = false;
                Arena.bLoggedWaitingForZone = false;
                const bool bNeededBothFlags =
                    !IsLiveObject(Arena.Flags[0]) &&
                    !IsLiveObject(Arena.Flags[1]);
                AAthenaBarrierFlag* SpawnedThisPass[2]{};
                if (!IsLiveObject(Arena.Flags[0]))
                {
                    SpawnedThisPass[0] =
                        SpawnDeepFriedFlag(
                            World,
                            Barrier,
                            FlagClass,
                            Placement.Locations[0],
                            PrefabYaw,
                            0);
                    Arena.Flags[0] =
                        SpawnedThisPass[0];
                }
                if (!IsLiveObject(Arena.Flags[1]))
                {
                    SpawnedThisPass[1] =
                        SpawnDeepFriedFlag(
                            World,
                            Barrier,
                            FlagClass,
                            Placement.Locations[1],
                            PrefabYaw + 180.0f,
                            1);
                    Arena.Flags[1] =
                        SpawnedThisPass[1];
                }

                if (bNeededBothFlags &&
                    (!IsLiveObject(Arena.Flags[0]) ||
                     !IsLiveObject(Arena.Flags[1])))
                {
                    // Keep pair placement transactional.  Retaining one
                    // successful flag while the safe-zone/grid inputs keep
                    // evolving would let the retry spawn its opponent around
                    // a different midpoint.
                    for (int32 TeamIndex = 0;
                         TeamIndex < 2;
                         ++TeamIndex)
                    {
                        if (IsLiveObject(
                                SpawnedThisPass[TeamIndex]))
                        {
                            SpawnedThisPass[TeamIndex]
                                ->K2_DestroyActor();
                        }
                        Arena.Flags[TeamIndex] = nullptr;
                    }
                }

                SDK::DbgLog(
                    "[FoodFight] floating base placement "
                    "anchor=%s:(%.1f,%.1f) pair=(%.1f,%.1f) "
                    "centerError=(%.1f,%.1f) "
                    "distance=%.1f yaw=%.0f "
                    "liftCells=%d liftZ=%.1f "
                    "traceStartZ=%.1f "
                    "groundZ=(%.1f,%.1f) delta=%.1f "
                    "maxDelta=%.1f "
                    "support=%s lavaZ=%.1f "
                    "buildableOffset=%.1f supportZ=%.1f "
                    "rootZ=(%.1f,%.1f) flags=(%p,%p)\n",
                    bDeepFried ? "zone" : "divider",
                    static_cast<float>(
                        PlacementAnchor.X),
                    static_cast<float>(
                        PlacementAnchor.Y),
                    static_cast<float>(
                        Placement.PairCenter.X),
                    static_cast<float>(
                        Placement.PairCenter.Y),
                    static_cast<float>(
                        Placement.PairCenter.X -
                        PlacementAnchor.X),
                    static_cast<float>(
                        Placement.PairCenter.Y -
                        PlacementAnchor.Y),
                    Distance,
                    PrefabYaw,
                    bDeepFried
                        ? 0
                        : NormalFoodFightSupportLiftCells,
                    SupportLiftZ,
                    TraceHeight,
                    static_cast<float>(
                        Placement.Grounds[0].Z),
                    static_cast<float>(
                        Placement.Grounds[1].Z),
                    Placement.GroundDifference,
                    MaxGroundDifference,
                    bDeepFried ? "lava" : "terrain",
                    FinalLavaZ,
                    BuildableOffset,
                    MinimumSupportZ,
                    static_cast<float>(
                        Placement.Locations[0].Z),
                    static_cast<float>(
                        Placement.Locations[1].Z),
                    static_cast<void*>(Arena.Flags[0]),
                    static_cast<void*>(Arena.Flags[1]));
            }
        }
        else if (IsLiveObject(FlagClass) &&
                 (!bHasPlayableZoneCenter ||
                  !bObjectivesMaySpawn) &&
                 (!IsLiveObject(Arena.Flags[0]) ||
                  !IsLiveObject(Arena.Flags[1])))
        {
            if (!Arena.bLoggedWaitingForZone)
            {
                Arena.bLoggedWaitingForZone = true;
                SDK::DbgLog(
                    "[FoodFight] deferred floating bases until "
                    "BusLocked and the first playable safe-zone "
                    "center are ready\n");
            }
        }

        RefreshDeepFriedArenaBindings();
        if (IsLiveObject(Arena.Wall))
            Arena.Wall->ForceNetUpdate();
        Barrier->ForceNetUpdate();
    }

    void TickDeepFriedArena(
        AFortGameStateAthena* GameState)
    {
        UWorld* World = UWorld::GetWorld();
        auto& Arena = GDeepFriedArenaState;
        if (!World || Arena.World != World ||
            !IsLiveObject(GameState))
        {
            return;
        }

        if (!IsLiveObject(Arena.Barrier))
        {
            const UClass* BarrierClass =
                AFortAthenaMutator_Barrier::StaticClass();
            AFortAthenaMutator_Barrier* Barrier = nullptr;
            if (BarrierClass &&
                GameState->HasGameplayMutators())
            {
                auto& Mutators = GameState->GameplayMutators;
                if (IsSaneArray(
                        Mutators.Num(), Mutators.Max(), 128) &&
                    IsReadableArrayStorage(
                        Mutators.Data,
                        Mutators.Num(),
                        sizeof(AFortGameplayMutator*)))
                {
                    for (auto Candidate : Mutators)
                    {
                        if (IsLiveObject(Candidate) &&
                            Candidate->IsA(BarrierClass))
                        {
                            Barrier =
                                static_cast<
                                    AFortAthenaMutator_Barrier*>(
                                        Candidate);
                            break;
                        }
                    }
                }
            }
            if (!Barrier && BarrierClass)
            {
                auto Candidate = FindWorldMutator(
                    const_cast<UClass*>(BarrierClass));
                if (IsLiveObject(Candidate) &&
                    Candidate->IsA(BarrierClass))
                {
                    Barrier =
                        static_cast<
                            AFortAthenaMutator_Barrier*>(
                                Candidate);
                }
            }
            if (!Barrier)
            {
                if (!Arena.bLoggedWaitingForBarrier)
                {
                    Arena.bLoggedWaitingForBarrier = true;
                    SDK::DbgLog(
                        "[FoodFight] waiting for the live Barrier "
                        "mutator before warmup wall creation\n");
                }
                return;
            }
            Arena.Barrier = Barrier;
            Arena.bLoggedWaitingForBarrier = false;
            SDK::DbgLog(
                "[FoodFight] resolved Barrier mutator=%p during "
                "warmup; creating the divider before BusLocked\n",
                static_cast<void*>(Barrier));
        }

        // Wall state and its player notifications are independent of the
        // objective/grid/lava prerequisites below. Poll every frame once the
        // wall exists so a deferred base placement cannot hide the short
        // ComingDown transition.
        RefreshDeepFriedArenaBindings();

        const bool bNeedsPlacement =
            !IsLiveObject(Arena.Wall) ||
            !IsLiveObject(Arena.Flags[0]) ||
            !IsLiveObject(Arena.Flags[1]);
        if (bNeedsPlacement)
        {
            const double Now =
                UGameplayStatics::GetTimeSeconds(World);
            if (Now >= Arena.NextPlacementRetryTime)
            {
                Arena.NextPlacementRetryTime =
                    Now + 1.0;
                EnsureDeepFriedArena(
                    World,
                    GameState,
                    Arena.Barrier,
                    Arena.SafeZoneInterface);
            }
            return;
        }

        RefreshDeepFriedArenaBindings();
    }

    bool IsCurrentArsenalGameState(
        AFortGameStateAthena* GameState)
    {
        UWorld* World = UWorld::GetWorld();
        if (!World || !IsLiveObject(GameState) ||
            std::fabs(
                VersionInfo.FortniteVersion -
                    NativeLTMVersion) >
                NativeLTMVersionTolerance)
        {
            return false;
        }

        auto IsArsenalPlaylist =
            [](const UFortPlaylistAthena* Playlist)
            {
                return IsNativeArsenalDescriptor(
                    FindExactNativeLTMDescriptor(Playlist));
            };

        if (GameState->HasCurrentPlaylistInfo())
        {
            if (FPlaylistPropertyArray::HasOverridePlaylist() &&
                IsArsenalPlaylist(
                    GameState->CurrentPlaylistInfo
                        .OverridePlaylist))
            {
                return true;
            }
            if (FPlaylistPropertyArray::HasBasePlaylist() &&
                IsArsenalPlaylist(
                    GameState->CurrentPlaylistInfo.BasePlaylist))
            {
                return true;
            }
        }
        if (GameState->HasCurrentPlaylistData() &&
            IsArsenalPlaylist(GameState->CurrentPlaylistData))
        {
            return true;
        }

        return GNativeLTMCompatibilityState.World == World &&
            IsNativeArsenalDescriptor(
                GNativeLTMCompatibilityState.Descriptor);
    }

    AFortAthenaMutator_GG* ResolveArsenalMutator(
        AFortGameStateAthena* GameState)
    {
        auto& State = GArsenalCompatibilityState;
        UWorld* World = UWorld::GetWorld();
        const UClass* GunGameClass =
            AFortAthenaMutator_GG::StaticClass();
        if (!World || !GameState || !GunGameClass)
            return nullptr;

        if (State.World != World)
        {
            State = {};
            State.World = World;
        }

        if (IsLiveObject(State.Mutator) &&
            State.Mutator->IsA(GunGameClass))
        {
            return State.Mutator;
        }

        AFortAthenaMutator_GG* Result = nullptr;
        if (GameState->HasGameplayMutators())
        {
            auto& Mutators = GameState->GameplayMutators;
            if (IsSaneArray(
                    Mutators.Num(), Mutators.Max(), 128) &&
                IsReadableArrayStorage(
                    Mutators.Data,
                    Mutators.Num(),
                    sizeof(AFortGameplayMutator*)))
            {
                for (auto Candidate : Mutators)
                {
                    if (IsLiveObject(Candidate) &&
                        Candidate->IsA(GunGameClass))
                    {
                        Result =
                            static_cast<AFortAthenaMutator_GG*>(
                                Candidate);
                        break;
                    }
                }
            }
        }

        if (!Result)
        {
            auto Candidate = FindWorldMutator(
                const_cast<UClass*>(GunGameClass));
            if (IsLiveObject(Candidate) &&
                Candidate->IsA(GunGameClass))
            {
                Result =
                    static_cast<AFortAthenaMutator_GG*>(
                        Candidate);
            }
        }

        if (!Result)
        {
            if (!State.bLoggedMissingMutator)
            {
                State.bLoggedMissingMutator = true;
                SDK::DbgLog(
                    "[Arsenal1040] waiting for the live "
                    "FortAthenaMutator_GG actor\n");
            }
            return nullptr;
        }

        if (State.Mutator != Result)
        {
            State.Mutator = Result;
            State.Tiers.clear();
            State.AllWeapons.clear();
            State.bTierTableReady = false;
            State.bLoggedMissingMutator = false;
            State.bLoggedInvalidTierData = false;
        }
        return Result;
    }

    bool BuildArsenalTierTable(
        AFortAthenaMutator_GG* Mutator)
    {
        auto& State = GArsenalCompatibilityState;
        if (State.bTierTableReady &&
            State.Mutator == Mutator)
        {
            return true;
        }

        State.Tiers.clear();
        State.AllWeapons.clear();

        const UStruct* EntryStruct =
            FGunGameGunEntry::StaticStruct();
        const UClass* WeaponClass =
            UFortWeaponItemDefinition::StaticClass();
        if (!IsLiveObject(Mutator) ||
            !EntryStruct || !WeaponClass ||
            !Mutator->HasWeaponEntries() ||
            !FGunGameGunEntry::HasWeapon() ||
            !FGunGameGunEntry::HasEnabled() ||
            !FGunGameGunEntry::HasAwardAtElim())
        {
            if (!State.bLoggedInvalidTierData)
            {
                State.bLoggedInvalidTierData = true;
                SDK::DbgLog(
                    "[Arsenal1040] reflected gun-game tier "
                    "properties are unavailable\n");
            }
            return false;
        }

        const int32 EntrySize =
            EntryStruct->GetPropertiesSize();
        auto& Entries = Mutator->WeaponEntries;
        if (EntrySize != 0x48 ||
            !IsSaneArray(
                Entries.Num(), Entries.Max(), 128) ||
            Entries.Num() == 0 ||
            !IsReadableArrayStorage(
                Entries.Data,
                Entries.Num(),
                static_cast<size_t>(EntrySize)))
        {
            if (!State.bLoggedInvalidTierData)
            {
                State.bLoggedInvalidTierData = true;
                SDK::DbgLog(
                    "[Arsenal1040] invalid WeaponEntries "
                    "layout size=0x%X num=%d max=%d\n",
                    EntrySize,
                    Entries.Num(),
                    Entries.Max());
            }
            return false;
        }

        for (int32 Index = 0;
             Index < Entries.Num(); ++Index)
        {
            auto& Entry = Entries.Get(Index, EntrySize);
            auto Weapon = Entry.Weapon;
            const float Enabled =
                Entry.Enabled.Evaluate(0.0f);
            const float AwardAtElim =
                Entry.AwardAtElim.Evaluate(0.0f);
            if (!IsLiveObject(Weapon) ||
                !Weapon->IsA(WeaponClass) ||
                !std::isfinite(Enabled) ||
                Enabled <= 0.0f ||
                !std::isfinite(AwardAtElim))
            {
                continue;
            }

            const float RoundedAward =
                std::round(AwardAtElim);
            if (std::fabs(
                    AwardAtElim - RoundedAward) > 0.05f ||
                RoundedAward < 0.0f ||
                RoundedAward > 1024.0f)
            {
                continue;
            }

            const int32 TierScore =
                static_cast<int32>(RoundedAward);
            auto& Tier = State.Tiers[TierScore];
            if (std::find(
                    Tier.begin(), Tier.end(), Weapon) ==
                Tier.end())
            {
                Tier.push_back(Weapon);
            }
            State.AllWeapons.insert(Weapon);
        }

        if (State.Tiers.empty())
        {
            if (!State.bLoggedInvalidTierData)
            {
                State.bLoggedInvalidTierData = true;
                SDK::DbgLog(
                    "[Arsenal1040] GG mutator had no enabled, "
                    "integral AwardAtElim weapon rows\n");
            }
            return false;
        }

        const float Reverse =
            Mutator->HasGameIsReverse()
                ? Mutator->GameIsReverse.Evaluate(0.0f)
                : 0.0f;
        const bool bReverseProgression =
            std::isfinite(Reverse) && Reverse > 0.5f;

        // AwardAtElim supplies the kill thresholds, while GameIsReverse
        // reverses which authored weapon group occupies each threshold. The
        // inverse 10.40 playlist therefore starts with the weapon authored at
        // the highest tier and finishes with the tier-zero weapon.
        if (bReverseProgression &&
            State.Tiers.size() > 1)
        {
            std::vector<
                std::vector<UFortWeaponItemDefinition*>>
                AuthoredWeaponGroups;
            AuthoredWeaponGroups.reserve(
                State.Tiers.size());
            for (const auto& [TierScore, Weapons] :
                 State.Tiers)
            {
                AuthoredWeaponGroups.push_back(Weapons);
            }

            size_t ReverseIndex =
                AuthoredWeaponGroups.size();
            for (auto& [TierScore, Weapons] :
                 State.Tiers)
            {
                Weapons =
                    AuthoredWeaponGroups[
                        --ReverseIndex];
            }
        }

        State.bTierTableReady = true;
        State.bLoggedInvalidTierData = false;

        const float FinalTierElims =
            Mutator->HasElimsWithFinalTierToWin()
                ? Mutator->ElimsWithFinalTierToWin
                      .Evaluate(0.0f)
                : 0.0f;
        const int32 ScoreToWin =
            Mutator->HasScoreToWin()
                ? Mutator->ScoreToWin
                : 0;
        SDK::DbgLog(
            "[Arsenal1040] loaded authored tier table "
            "mutator=%s entries=%d tiers=%llu range=%d..%d "
            "reverse=%.1f progression=%s "
            "finalTierElims=%.1f scoreToWin=%d\n",
            Mutator->Name.ToString().c_str(),
            Entries.Num(),
            static_cast<unsigned long long>(
                State.Tiers.size()),
            State.Tiers.begin()->first,
            State.Tiers.rbegin()->first,
            static_cast<double>(Reverse),
            bReverseProgression
                ? "descending"
                : "ascending",
            static_cast<double>(FinalTierElims),
            ScoreToWin);

        for (const auto& [TierScore, Weapons] :
             State.Tiers)
        {
            std::string WeaponNames;
            for (auto Weapon : Weapons)
            {
                if (!WeaponNames.empty())
                    WeaponNames += ",";
                WeaponNames +=
                    Weapon
                        ? Weapon->Name.ToString()
                        : "<null>";
            }
            SDK::DbgLog(
                "[Arsenal1040] tier=%d weapons=%s\n",
                TierScore, WeaponNames.c_str());
        }
        return true;
    }

    void ResolveArsenalGrantAmmo(
        UFortWeaponItemDefinition* WeaponDefinition,
        int32& OutLoadedAmmo,
        int32& OutPhantomReserveAmmo)
    {
        OutLoadedAmmo = 0;
        OutPhantomReserveAmmo = 0;
        if (!WeaponDefinition)
            return;

        auto Stats =
            AFortInventory::GetStats(WeaponDefinition);
        if (!Stats)
            return;

        OutLoadedAmmo = (std::max)(Stats->ClipSize, 0);
        if (WeaponDefinition
                ->HasbUsesPhantomReserveAmmo() &&
            WeaponDefinition->bUsesPhantomReserveAmmo)
        {
            OutPhantomReserveAmmo =
                (std::max)(
                    (Stats->InitialClips - 1) *
                        OutLoadedAmmo,
                    0);
        }
    }

    bool RepairArsenalEntryAmmo(
        AFortInventory* Inventory,
        FFortItemEntry& Entry,
        UFortWeaponItemDefinition* WeaponDefinition)
    {
        if (!Inventory || !WeaponDefinition)
            return false;

        int32 LoadedAmmo = 0;
        int32 PhantomReserveAmmo = 0;
        ResolveArsenalGrantAmmo(
            WeaponDefinition,
            LoadedAmmo,
            PhantomReserveAmmo);

        bool bChanged = false;
        if (LoadedAmmo > 0 &&
            Entry.LoadedAmmo != LoadedAmmo)
        {
            Entry.LoadedAmmo = LoadedAmmo;
            bChanged = true;
        }
        if (Entry.HasPhantomReserveAmmo() &&
            Entry.PhantomReserveAmmo !=
                PhantomReserveAmmo)
        {
            Entry.PhantomReserveAmmo =
                PhantomReserveAmmo;
            bChanged = true;
        }
        if (!bChanged)
            return false;

        const FGuid Guid = Entry.ItemGuid;
        auto ItemInstance =
            Inventory->Inventory.ItemInstances.Search(
                [&](UFortWorldItem* Item)
                {
                    return Item &&
                        Item->ItemEntry.ItemGuid.A == Guid.A &&
                        Item->ItemEntry.ItemGuid.B == Guid.B &&
                        Item->ItemEntry.ItemGuid.C == Guid.C &&
                        Item->ItemEntry.ItemGuid.D == Guid.D;
                });
        if (ItemInstance && *ItemInstance)
        {
            (*ItemInstance)->ItemEntry.LoadedAmmo =
                Entry.LoadedAmmo;
            if ((*ItemInstance)->ItemEntry
                    .HasPhantomReserveAmmo())
            {
                (*ItemInstance)->ItemEntry
                    .PhantomReserveAmmo =
                    Entry.HasPhantomReserveAmmo()
                        ? Entry.PhantomReserveAmmo
                        : 0;
            }
            (*ItemInstance)->ItemEntry.bIsDirty = true;
        }
        Inventory->Update(&Entry);
        return true;
    }

    int32 GetArsenalKillScore(
        AFortPlayerStateAthena* PlayerState)
    {
        if (!PlayerState)
            return 0;
        if (PlayerState->HasKillScore())
            return (std::max)(PlayerState->KillScore, 0);
        if (PlayerState->HasKills())
            return (std::max)(PlayerState->Kills, 0);
        return 0;
    }

    bool ReconcileArsenalPlayerTier(
        AFortPlayerControllerAthena* PlayerController,
        int32 EliminationScore,
        const char* Reason)
    {
        UWorld* World = UWorld::GetWorld();
        auto GameState =
            World && World->GameState
                ? World->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        if (!World ||
            !IsCurrentArsenalGameState(GameState) ||
            !IsLiveObject(PlayerController) ||
            !PlayerController->HasPlayerState() ||
            !IsLiveObject(PlayerController->PlayerState) ||
            !IsLiveObject(PlayerController->WorldInventory) ||
            !PlayerController->HasMyFortPawn() ||
            !IsLiveObject(PlayerController->MyFortPawn))
        {
            return false;
        }

        auto PlayerState =
            static_cast<AFortPlayerStateAthena*>(
                PlayerController->PlayerState);
        auto Mutator = ResolveArsenalMutator(GameState);
        if (!Mutator || !BuildArsenalTierTable(Mutator))
            return false;

        auto& State = GArsenalCompatibilityState;
        auto& PlayerData = State.Players[PlayerState];
        const int32 EffectiveScore =
            (std::max)(EliminationScore, 0);

        auto TierIterator =
            State.Tiers.upper_bound(EffectiveScore);
        if (TierIterator == State.Tiers.begin())
        {
            TierIterator = State.Tiers.begin();
        }
        else
        {
            --TierIterator;
        }

        const int32 TierScore = TierIterator->first;
        const auto& DesiredWeapons =
            TierIterator->second;
        if (DesiredWeapons.empty())
            return false;

        auto Inventory = PlayerController->WorldInventory;
        auto& ReplicatedEntries =
            Inventory->Inventory.ReplicatedEntries;
        const int32 ItemEntrySize =
            FFortItemEntry::Size();
        if (ItemEntrySize <= 0 ||
            ItemEntrySize > 0x400 ||
            !IsSaneArray(
                ReplicatedEntries.Num(),
                ReplicatedEntries.Max(),
                512) ||
            !IsReadableArrayStorage(
                ReplicatedEntries.Data,
                ReplicatedEntries.Num(),
                static_cast<size_t>(ItemEntrySize)))
        {
            return false;
        }

        const bool bTierOrPawnChanged =
            PlayerData.AppliedTierScore != TierScore ||
            PlayerData.Pawn != PlayerController->MyFortPawn ||
            PlayerData.AssignedWeapons != DesiredWeapons;

        std::unordered_map<
            UFortWeaponItemDefinition*, FGuid>
            DesiredGuids;
        std::vector<FGuid> GuidsToRemove;
        int32 AmmoRepairs = 0;

        for (int32 Index = 0;
             Index < ReplicatedEntries.Num(); ++Index)
        {
            auto& Entry = ReplicatedEntries.Get(
                Index, ItemEntrySize);
            auto AuthoredWeapon =
                const_cast<UFortWeaponItemDefinition*>(
                    reinterpret_cast<
                        const UFortWeaponItemDefinition*>(
                            Entry.ItemDefinition));
            if (!State.AllWeapons.contains(
                    AuthoredWeapon))
            {
                continue;
            }

            const bool bDesired =
                std::find(
                    DesiredWeapons.begin(),
                    DesiredWeapons.end(),
                    AuthoredWeapon) !=
                DesiredWeapons.end();
            if (bDesired &&
                !DesiredGuids.contains(AuthoredWeapon))
            {
                DesiredGuids.emplace(
                    AuthoredWeapon, Entry.ItemGuid);
                if (bTierOrPawnChanged &&
                    RepairArsenalEntryAmmo(
                        Inventory,
                        Entry,
                        AuthoredWeapon))
                {
                    ++AmmoRepairs;
                }
            }
            else
            {
                GuidsToRemove.push_back(Entry.ItemGuid);
            }
        }

        for (const FGuid& Guid : GuidsToRemove)
            Inventory->Remove(Guid);

        int32 AddedWeapons = 0;
        for (auto Weapon : DesiredWeapons)
        {
            if (DesiredGuids.contains(Weapon))
                continue;

            int32 LoadedAmmo = 0;
            int32 PhantomReserveAmmo = 0;
            ResolveArsenalGrantAmmo(
                Weapon,
                LoadedAmmo,
                PhantomReserveAmmo);
            auto Item = Inventory->GiveItem(
                Weapon,
                1,
                LoadedAmmo,
                0,
                true,
                true,
                PhantomReserveAmmo);
            if (!Item)
                continue;

            DesiredGuids.emplace(
                Weapon, Item->ItemEntry.ItemGuid);
            ++AddedWeapons;
        }

        if (DesiredGuids.size() !=
            DesiredWeapons.size())
        {
            SDK::DbgLog(
                "[Arsenal1040] tier reconciliation "
                "incomplete PC=%p score=%d tier=%d "
                "desired=%llu present=%llu\n",
                static_cast<void*>(PlayerController),
                EffectiveScore,
                TierScore,
                static_cast<unsigned long long>(
                    DesiredWeapons.size()),
                static_cast<unsigned long long>(
                    DesiredGuids.size()));
            return false;
        }

        const bool bInventoryChanged =
            !GuidsToRemove.empty() ||
            AddedWeapons > 0 ||
            AmmoRepairs > 0;
        if (bTierOrPawnChanged || bInventoryChanged)
        {
            auto EquipGuid =
                DesiredGuids.find(DesiredWeapons.front());
            if (EquipGuid != DesiredGuids.end())
            {
                PlayerController
                    ->ServerExecuteInventoryItem(
                        EquipGuid->second);
                PlayerController->ClientEquipItem(
                    EquipGuid->second, true);
            }
        }

        const bool bProgressChanged =
            PlayerData.AppliedEliminationScore !=
                EffectiveScore;
        PlayerData.AppliedEliminationScore =
            EffectiveScore;
        PlayerData.AppliedTierScore = TierScore;
        PlayerData.Pawn =
            PlayerController->MyFortPawn;
        PlayerData.AssignedWeapons = DesiredWeapons;

        if (bTierOrPawnChanged ||
            bInventoryChanged ||
            bProgressChanged)
        {
            SDK::DbgLog(
                "[Arsenal1040] synchronized %s PC=%p "
                "PS=%p eliminations=%d tier=%d "
                "weapons=%llu removed=%llu added=%d "
                "ammoRepaired=%d pawnChanged=%d\n",
                Reason ? Reason : "player",
                static_cast<void*>(PlayerController),
                static_cast<void*>(PlayerState),
                EffectiveScore,
                TierScore,
                static_cast<unsigned long long>(
                    DesiredWeapons.size()),
                static_cast<unsigned long long>(
                    GuidsToRemove.size()),
                AddedWeapons,
                AmmoRepairs,
                bTierOrPawnChanged ? 1 : 0);
        }
        return true;
    }

    bool IsArsenalGameplayPhase(
        AFortGameStateAthena* GameState,
        bool bAllowAircraft)
    {
        if (!GameState ||
            !GameState->HasGamePhase())
        {
            return false;
        }

        const uint8 MinimumPhase =
            static_cast<uint8>(
                bAllowAircraft
                    ? EAthenaGamePhase::Aircraft
                    : EAthenaGamePhase::SafeZones);
        return GameState->GamePhase >= MinimumPhase &&
            GameState->GamePhase <
                static_cast<uint8>(
                    EAthenaGamePhase::EndGame);
    }

    void TickArsenalCompatibility(
        AFortGameStateAthena* GameState,
        double Now)
    {
        auto& State = GArsenalCompatibilityState;
        if (!IsCurrentArsenalGameState(GameState) ||
            !IsArsenalGameplayPhase(GameState, false) ||
            Now < State.NextReconcileTime)
        {
            return;
        }
        State.NextReconcileTime = Now + 0.50;

        UWorld* World = UWorld::GetWorld();
        auto GameMode =
            World && World->AuthorityGameMode
                ? World->AuthorityGameMode
                      ->Cast<AFortGameMode>()
                : nullptr;
        if (!GameMode)
            return;

        for (auto UncastedController :
             GameMode->AlivePlayers)
        {
            auto PlayerController =
                static_cast<
                    AFortPlayerControllerAthena*>(
                        UncastedController);
            if (!IsLiveObject(PlayerController) ||
                !PlayerController->HasPlayerState() ||
                !IsLiveObject(
                    PlayerController->PlayerState))
            {
                continue;
            }

            auto PlayerState =
                static_cast<AFortPlayerStateAthena*>(
                    PlayerController->PlayerState);
            const int32 EffectiveScore =
                GetArsenalKillScore(PlayerState);
            ReconcileArsenalPlayerTier(
                PlayerController,
                EffectiveScore,
                "watchdog");
        }
    }

    bool IsCurrentNormalBountyGameState(
        AFortGameStateAthena* GameState)
    {
        UWorld* World = UWorld::GetWorld();
        if (!World || !IsLiveObject(GameState) ||
            std::fabs(
                VersionInfo.FortniteVersion -
                    NativeLTMVersion) >
                NativeLTMVersionTolerance)
        {
            return false;
        }

        bool bHasAuthoritativePlaylist = false;
        auto CheckAuthoritativePlaylist =
            [&](const UFortPlaylistAthena* Playlist)
            {
                if (!IsLiveObject(Playlist))
                    return false;

                bHasAuthoritativePlaylist = true;
                return IsNativeBountyDescriptor(
                    FindExactNativeLTMDescriptor(Playlist));
            };

        if (GameState->HasCurrentPlaylistInfo())
        {
            if (FPlaylistPropertyArray::HasOverridePlaylist() &&
                CheckAuthoritativePlaylist(
                    GameState->CurrentPlaylistInfo
                        .OverridePlaylist))
            {
                return true;
            }
            if (FPlaylistPropertyArray::HasBasePlaylist() &&
                CheckAuthoritativePlaylist(
                    GameState->CurrentPlaylistInfo.BasePlaylist))
            {
                return true;
            }
        }
        if (GameState->HasCurrentPlaylistData() &&
            CheckAuthoritativePlaylist(
                GameState->CurrentPlaylistData))
        {
            return true;
        }

        if (bHasAuthoritativePlaylist)
            return false;

        return GNativeLTMCompatibilityState.World == World &&
            IsNativeBountyDescriptor(
                GNativeLTMCompatibilityState.Descriptor);
    }

    bool IsCurrentWaxGameState(
        AFortGameStateAthena* GameState)
    {
        UWorld* World = UWorld::GetWorld();
        if (!World || !IsLiveObject(GameState) ||
            std::fabs(
                VersionInfo.FortniteVersion -
                    NativeLTMVersion) >
                NativeLTMVersionTolerance)
        {
            return false;
        }

        auto IsWaxPlaylist =
            [](const UFortPlaylistAthena* Playlist)
            {
                return IsNativeWaxDescriptor(
                    FindExactNativeLTMDescriptor(Playlist));
            };

        bool bHasAuthoritativePlaylist = false;
        auto CheckAuthoritativePlaylist =
            [&](const UFortPlaylistAthena* Playlist)
            {
                if (!IsLiveObject(Playlist))
                    return false;

                bHasAuthoritativePlaylist = true;
                return IsWaxPlaylist(Playlist);
            };

        if (GameState->HasCurrentPlaylistInfo())
        {
            if (FPlaylistPropertyArray::HasOverridePlaylist() &&
                CheckAuthoritativePlaylist(
                    GameState->CurrentPlaylistInfo
                        .OverridePlaylist))
            {
                return true;
            }
            if (FPlaylistPropertyArray::HasBasePlaylist() &&
                CheckAuthoritativePlaylist(
                    GameState->CurrentPlaylistInfo.BasePlaylist))
            {
                return true;
            }
        }
        if (GameState->HasCurrentPlaylistData() &&
            CheckAuthoritativePlaylist(
                GameState->CurrentPlaylistData))
        {
            return true;
        }

        // Never let the previous match's compatibility descriptor override a
        // live, authoritative non-Wax playlist during world reuse.
        if (bHasAuthoritativePlaylist)
            return false;

        return GNativeLTMCompatibilityState.World == World &&
            IsNativeWaxDescriptor(
                GNativeLTMCompatibilityState.Descriptor);
    }

    bool IsWaxGameplayPhase(
        AFortGameStateAthena* GameState)
    {
        return GameState &&
            GameState->HasGamePhase() &&
            GameState->GamePhase >=
                static_cast<uint8>(
                    EAthenaGamePhase::Aircraft) &&
            GameState->GamePhase <
                static_cast<uint8>(
                    EAthenaGamePhase::EndGame);
    }

    void EnsureNormalBountyTokenClasses(
        AFortAthenaMutator_Wax* Mutator)
    {
        auto& State = GWaxCompatibilityState;
        if (State.bBountyTokenClassesVerified)
            return;

        const UClass* BountyMutatorClass =
            FindObject<UClass>(
                L"/Game/Athena/Playlists/Wax/"
                L"Mutator_Bounty.Mutator_Bounty_C");
        const UClass* BountyTokenClass =
            FindObject<UClass>(
                L"/Game/Athena/Playlists/Wax/"
                L"BountyToken.BountyToken_C");
        const UClass* BountyPickupClass =
            FindObject<UClass>(
                L"/Game/Athena/Playlists/Wax/"
                L"BountyPickup.BountyPickup_C");
        const UClass* WaxTokenBaseClass =
            FindClass("FortAthena_WaxToken");
        const UClass* WaxPickupBaseClass =
            AFortGameModePickup_Wax::StaticClass();

        const bool bClassesValid =
            IsLiveObject(Mutator) &&
            IsLiveObject(BountyMutatorClass) &&
            Mutator->IsA(BountyMutatorClass) &&
            IsLiveObject(BountyTokenClass) &&
            IsLiveObject(BountyPickupClass) &&
            IsLiveObject(WaxTokenBaseClass) &&
            IsLiveObject(WaxPickupBaseClass) &&
            IsLiveObject(BountyTokenClass->GetDefaultObj()) &&
            BountyTokenClass->GetDefaultObj()->IsA(
                WaxTokenBaseClass) &&
            IsLiveObject(BountyPickupClass->GetDefaultObj()) &&
            BountyPickupClass->GetDefaultObj()->IsA(
                WaxPickupBaseClass);
        const bool bLayoutValid =
            Mutator &&
            Mutator->HasTokenClass() &&
            Mutator->HasTokenPickupClass() &&
            AFortAthenaMutator_Wax::TokenClass__Offset ==
                0x2E8 &&
            AFortAthenaMutator_Wax::TokenPickupClass__Offset ==
                0x2F0;
        if (!bClassesValid || !bLayoutValid)
        {
            if (!State.bLoggedInvalidBountyTokenClasses)
            {
                State.bLoggedInvalidBountyTokenClasses = true;
                SDK::DbgLog(
                    "[Bounty1040] waiting for the exact normal "
                    "Bounty token classes/layout classesValid=%d "
                    "layoutValid=%d\n",
                    bClassesValid ? 1 : 0,
                    bLayoutValid ? 1 : 0);
            }
            return;
        }

        auto& TokenClass = Mutator->GetTokenClass();
        auto& TokenPickupClass =
            Mutator->GetTokenPickupClass();
        const UClass* PreviousTokenClass =
            TokenClass.Get();
        const UClass* PreviousTokenPickupClass =
            TokenPickupClass.Get();
        const bool bRepairedTokenClass =
            PreviousTokenClass != BountyTokenClass;
        const bool bRepairedTokenPickupClass =
            PreviousTokenPickupClass != BountyPickupClass;
        if (bRepairedTokenClass)
            TokenClass.ClassPtr = BountyTokenClass;
        if (bRepairedTokenPickupClass)
            TokenPickupClass.ClassPtr = BountyPickupClass;

        State.bBountyTokenClassesVerified = true;
        SDK::DbgLog(
            "[Bounty1040] exact normal Bounty coin classes "
            "verified token=%p pickup=%p repairedToken=%d "
            "repairedPickup=%d previousToken=%p "
            "previousPickup=%p\n",
            static_cast<const void*>(BountyTokenClass),
            static_cast<const void*>(BountyPickupClass),
            bRepairedTokenClass ? 1 : 0,
            bRepairedTokenPickupClass ? 1 : 0,
            static_cast<const void*>(PreviousTokenClass),
            static_cast<const void*>(PreviousTokenPickupClass));
    }

    AFortAthenaMutator_Wax* ResolveWaxMutator(
        AFortGameStateAthena* GameState)
    {
        auto& State = GWaxCompatibilityState;
        UWorld* World = UWorld::GetWorld();
        const UClass* WaxClass =
            AFortAthenaMutator_Wax::StaticClass();
        const bool bNormalBounty =
            IsCurrentNormalBountyGameState(GameState);
        const UClass* ExpectedBountyMutatorClass =
            bNormalBounty
                ? FindObject<UClass>(
                      L"/Game/Athena/Playlists/Wax/"
                      L"Mutator_Bounty.Mutator_Bounty_C")
                : nullptr;
        if (!World || !GameState || !WaxClass)
            return nullptr;

        if (State.World != World)
        {
            State = {};
            State.World = World;
        }

        if (IsLiveObject(State.Mutator) &&
            State.Mutator->IsA(WaxClass) &&
            (!bNormalBounty ||
             (ExpectedBountyMutatorClass &&
              State.Mutator->IsA(
                  ExpectedBountyMutatorClass))))
        {
            if (bNormalBounty)
            {
                EnsureNormalBountyTokenClasses(
                    State.Mutator);
            }
            return State.Mutator;
        }

        AFortAthenaMutator_Wax* Result = nullptr;
        if (GameState->HasGameplayMutators())
        {
            auto& Mutators = GameState->GameplayMutators;
            if (IsSaneArray(
                    Mutators.Num(), Mutators.Max(), 128) &&
                IsReadableArrayStorage(
                    Mutators.Data,
                    Mutators.Num(),
                    sizeof(AFortGameplayMutator*)))
            {
                for (auto Candidate : Mutators)
                {
                    if (IsLiveObject(Candidate) &&
                        Candidate->IsA(WaxClass) &&
                        (!bNormalBounty ||
                         (ExpectedBountyMutatorClass &&
                          Candidate->IsA(
                              ExpectedBountyMutatorClass))))
                    {
                        Result =
                            static_cast<
                                AFortAthenaMutator_Wax*>(
                                    Candidate);
                        break;
                    }
                }
            }
        }

        if (!Result)
        {
            const UClass* SearchClass =
                bNormalBounty
                    ? ExpectedBountyMutatorClass
                    : WaxClass;
            auto Candidate =
                SearchClass
                    ? FindWorldMutator(
                          const_cast<UClass*>(SearchClass))
                    : nullptr;
            if (IsLiveObject(Candidate) &&
                Candidate->IsA(WaxClass) &&
                (!bNormalBounty ||
                 Candidate->IsA(
                     ExpectedBountyMutatorClass)))
            {
                Result =
                    static_cast<AFortAthenaMutator_Wax*>(
                        Candidate);
            }
        }

        if (!Result)
        {
            if (!State.bLoggedMissingMutator)
            {
                State.bLoggedMissingMutator = true;
                if (bNormalBounty)
                {
                    SDK::DbgLog(
                        "[Bounty1040] waiting for the exact live "
                        "Mutator_Bounty_C actor\n");
                }
                else
                {
                    SDK::DbgLog(
                        "[Wax1040] waiting for the live "
                        "FortAthenaMutator_Wax actor\n");
                }
            }
            return nullptr;
        }

        if (State.Mutator != Result)
        {
            const double Now =
                UGameplayStatics::GetTimeSeconds(World);
            State = {};
            State.World = World;
            State.Mutator = Result;
            State.MutatorResolvedTime = Now;
        }
        if (bNormalBounty)
        {
            EnsureNormalBountyTokenClasses(Result);
        }
        return Result;
    }

    using FWaxCollectTokensNative = void (*)(
        AFortAthenaMutator_Wax*,
        AFortPlayerPawnAthena*,
        int32);

    FWaxCollectTokensNative ResolveWaxCollectTokensNative()
    {
        static bool bInitialized = false;
        static FWaxCollectTokensNative Native = nullptr;
        if (bInitialized)
            return Native;
        bInitialized = true;

        if (!FFortAthenaNativeLTMCompatibility::
                IsSupportedBuild())
        {
            return nullptr;
        }

        auto DefaultPickup =
            AFortGameModePickup_Wax::GetDefaultObj();
        FNativeCodeRange TextRange{};
        if (!DefaultPickup || !DefaultPickup->Vft ||
            !GetNativeImageTextRange(TextRange))
        {
            return nullptr;
        }

        // Both 10.40 Wax collection virtuals load AmountOfTokens from
        // 0x564 and call the same private mutator scorer. Resolve that call
        // from the live vtable instead of pinning an ASLR-sensitive address.
        static constexpr int32 CandidateSlots[] = {
            195,
            204
        };
        static constexpr uint8 Prefixes[][14] = {
            {
                0x44, 0x8B, 0x83, 0x64, 0x05, 0x00, 0x00,
                0x48, 0x8B, 0xD7,
                0x48, 0x8B, 0xCE,
                0xE8
            },
            {
                0x44, 0x8B, 0x83, 0x64, 0x05, 0x00, 0x00,
                0x48, 0x8B, 0xD6,
                0x48, 0x8B, 0xCD,
                0xE8
            }
        };

        uintptr_t ResolvedTarget = 0;
        int32 MatchingVirtuals = 0;
        for (int32 CandidateIndex = 0;
             CandidateIndex <
                 static_cast<int32>(
                     std::size(CandidateSlots));
             ++CandidateIndex)
        {
            const int32 Slot =
                CandidateSlots[CandidateIndex];
            const auto& Prefix =
                Prefixes[CandidateIndex];
            if (!SDK::MemReadable(
                    DefaultPickup->Vft + Slot,
                    sizeof(void*)))
            {
                continue;
            }

            const uintptr_t Virtual =
                reinterpret_cast<uintptr_t>(
                    DefaultPickup->Vft[Slot]);
            FNativeCodeRange FunctionRange{};
            if (!GetRuntimeFunctionRange(
                    Virtual, TextRange, FunctionRange))
            {
                continue;
            }

            const uintptr_t ScanStart =
                (std::max)(Virtual, FunctionRange.Start);
            const uintptr_t ScanEnd =
                (std::min)(
                    FunctionRange.End,
                    ScanStart + 0x400);
            for (uintptr_t Cursor = ScanStart;
                 Cursor + sizeof(Prefix) + sizeof(int32) <=
                     ScanEnd;
                 ++Cursor)
            {
                if (std::memcmp(
                        reinterpret_cast<const void*>(Cursor),
                        Prefix,
                        sizeof(Prefix)) != 0)
                {
                    continue;
                }

                const uintptr_t Target =
                    ResolveRelativeBranchTarget(
                        Cursor + sizeof(Prefix) - 1,
                        5,
                        1);
                if (!IsAddressInCodeRange(
                        Target, 16, TextRange) ||
                    IsNativeEmptyVoidStub(
                        Target, TextRange))
                {
                    continue;
                }

                if (ResolvedTarget &&
                    ResolvedTarget != Target)
                {
                    SDK::DbgLog(
                        "[Wax1040] rejected conflicting token "
                        "scorers first=%p second=%p\n",
                        reinterpret_cast<void*>(ResolvedTarget),
                        reinterpret_cast<void*>(Target));
                    return nullptr;
                }
                ResolvedTarget = Target;
                ++MatchingVirtuals;
                break;
            }
        }

        static constexpr const char* CallSitePatterns[] = {
            "44 8B 83 64 05 00 00 48 8B D7 48 8B CE "
            "E8 ? ? ? ?",
            "44 8B 83 64 05 00 00 48 8B D6 48 8B CD "
            "E8 ? ? ? ?"
        };
        int32 MatchingGlobalCallSites = 0;
        if (!ResolvedTarget ||
            MatchingVirtuals !=
                static_cast<int32>(
                    std::size(CandidateSlots)))
        {
            // The stripped 10.40 server can expose a pickup CDO whose live
            // vtable no longer maps to the two client-derived slots above.
            // The call sites themselves remain unique in .text, so use them
            // as the ASLR-safe fallback and still require both independent
            // callers to agree on one non-stub scorer.
            uintptr_t GlobalTarget = 0;
            for (const char* Pattern : CallSitePatterns)
            {
                const uintptr_t CallSite =
                    Memcury::Scanner::FindPattern(Pattern).Get();
                if (!CallSite ||
                    !IsAddressInCodeRange(
                        CallSite, 18, TextRange))
                {
                    continue;
                }

                const uintptr_t Target =
                    ResolveRelativeBranchTarget(
                        CallSite + 13,
                        5,
                        1);
                if (!IsAddressInCodeRange(
                        Target, 16, TextRange) ||
                    IsNativeEmptyVoidStub(
                        Target, TextRange))
                {
                    continue;
                }

                if (GlobalTarget &&
                    GlobalTarget != Target)
                {
                    SDK::DbgLog(
                        "[Wax1040] rejected conflicting global "
                        "token scorers first=%p second=%p\n",
                        reinterpret_cast<void*>(GlobalTarget),
                        reinterpret_cast<void*>(Target));
                    return nullptr;
                }
                GlobalTarget = Target;
                ++MatchingGlobalCallSites;
            }

            if (GlobalTarget &&
                MatchingGlobalCallSites ==
                    static_cast<int32>(
                        std::size(CallSitePatterns)))
            {
                ResolvedTarget = GlobalTarget;
            }
        }

        if (ResolvedTarget &&
            (MatchingVirtuals ==
                 static_cast<int32>(
                     std::size(CandidateSlots)) ||
             MatchingGlobalCallSites ==
                 static_cast<int32>(
                     std::size(CallSitePatterns))))
        {
            Native =
                reinterpret_cast<FWaxCollectTokensNative>(
                    ResolvedTarget);
        }
        SDK::DbgLog(
            "[Wax1040] native token scorer=%p "
            "virtualMatches=%d globalMatches=%d\n",
            reinterpret_cast<void*>(ResolvedTarget),
            MatchingVirtuals,
            MatchingGlobalCallSites);
        return Native;
    }

    bool ValidateWaxPlayerData(
        AFortAthenaMutator_Wax* Mutator,
        int32& OutEntrySize)
    {
        OutEntrySize = 0;
        const UStruct* EntryStruct =
            FWaxPlayerDataEntry::StaticStruct();
        const UStruct* ArrayStruct =
            FWaxPlayerDataArray::StaticStruct();
        if (!IsLiveObject(Mutator) ||
            !EntryStruct || !ArrayStruct ||
            !Mutator->HasPlayerData() ||
            !FWaxPlayerDataArray::HasOwningMutator() ||
            !FWaxPlayerDataArray::HasEntries() ||
            !FWaxPlayerDataEntry::HasPlayerState() ||
            !FWaxPlayerDataEntry::HasbPermanentlyWaxed() ||
            !FWaxPlayerDataEntry::HasbPlayerWasLeader() ||
            !FWaxPlayerDataEntry::HasTokenBasedPlacement() ||
            !FWaxPlayerDataEntry::HasCurrentTokens() ||
            !FWaxPlayerDataEntry::HasPreviousTokens() ||
            !FWaxPlayerDataEntry::HasCurrentTeamTokens() ||
            !FWaxPlayerDataEntry::HasPreviousTeamTokens() ||
            !FWaxPlayerDataEntry::HasCurrentKills() ||
            !FWaxPlayerDataEntry::HasPreviousKills() ||
            !FWaxPlayerDataEntry::HasCurrentLives() ||
            !FWaxPlayerDataEntry::HasPreviousVictimLocation())
        {
            return false;
        }

        const int32 EntrySize =
            EntryStruct->GetPropertiesSize();
        const int32 ArraySize =
            ArrayStruct->GetPropertiesSize();
        FReflectedPropertySpan PlayerDataSpan;
        if (EntrySize != 0x48 ||
            ArraySize != 0x120 ||
            !ResolveReflectedPropertySpan(
                Mutator->Class,
                "PlayerData",
                static_cast<uint32>(ArraySize),
                PlayerDataSpan))
        {
            return false;
        }

        auto& Entries = Mutator->PlayerData.Entries;
        if (!IsSaneArray(
                Entries.Num(), Entries.Max(), 256) ||
            !IsReadableArrayStorage(
                Entries.GetData(),
                Entries.Num(),
                static_cast<size_t>(EntrySize)))
        {
            return false;
        }

        OutEntrySize = EntrySize;
        return true;
    }

    FWaxPlayerDataEntry* FindWaxPlayerDataEntry(
        AFortAthenaMutator_Wax* Mutator,
        AFortPlayerStateAthena* PlayerState,
        int32 EntrySize)
    {
        if (!Mutator || !PlayerState || EntrySize <= 0)
            return nullptr;

        auto& Entries = Mutator->PlayerData.Entries;
        for (int32 Index = 0;
             Index < Entries.Num();
             ++Index)
        {
            auto& Entry = Entries.Get(Index, EntrySize);
            if (Entry.PlayerState == PlayerState)
                return &Entry;
        }
        return nullptr;
    }

    AFortPlayerStateAthena* ResolveWaxPawnPlayerState(
        AFortPlayerPawnAthena* Pawn)
    {
        if (!IsLiveObject(Pawn))
            return nullptr;

        AActor* Candidate =
            Pawn->HasPlayerState() ? Pawn->PlayerState : nullptr;
        auto PlayerState =
            IsLiveObject(Candidate)
                ? Candidate->Cast<AFortPlayerStateAthena>()
                : nullptr;
        if (PlayerState)
            return PlayerState;

        auto Controller =
            Pawn->HasController() && IsLiveObject(Pawn->Controller)
                ? Pawn->Controller
                      ->Cast<AFortPlayerControllerAthena>()
                : nullptr;
        return Controller &&
                Controller->HasPlayerState() &&
                IsLiveObject(Controller->PlayerState)
            ? static_cast<AFortPlayerStateAthena*>(
                  Controller->PlayerState)
            : nullptr;
    }

    bool IsPlayableWaxPawn(AFortPlayerPawnAthena* Pawn)
    {
        return IsLiveObject(Pawn) &&
            !Pawn->IsDBNO() &&
            (!Pawn->HasbIsDying() || !Pawn->bIsDying);
    }

    bool IsWaxEliminationProcessed(
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn)
    {
        auto& Claims =
            GWaxCompatibilityState.ProcessedEliminationPawns;
        auto Existing = Claims.find(PlayerState);
        return Existing != Claims.end() &&
            Existing->second.Get() == Pawn;
    }

    bool TryClaimWaxElimination(
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn)
    {
        if (!IsLiveObject(PlayerState) ||
            !IsLiveObject(Pawn) ||
            IsWaxEliminationProcessed(PlayerState, Pawn))
        {
            return false;
        }

        GWaxCompatibilityState
            .ProcessedEliminationPawns[PlayerState] =
            TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
        return true;
    }

    double ObserveMissingWaxPlayerData(
        AFortPlayerStateAthena* PlayerState,
        double Now)
    {
        auto& Missing =
            GWaxCompatibilityState.MissingPlayerDataSince;
        auto [Iterator, Inserted] =
            Missing.emplace(PlayerState, Now);
        return Iterator->second;
    }

    bool WaxPlayerDataGraceElapsed(
        AFortPlayerStateAthena* PlayerState,
        double Now)
    {
        return Now >=
            ObserveMissingWaxPlayerData(PlayerState, Now) +
                WaxPlayerDataGraceSeconds;
    }

    bool ResolveWaxInitialValues(
        AFortAthenaMutator_Wax* Mutator,
        int32& OutTokens,
        int32& OutLives)
    {
        OutTokens = 0;
        OutLives = 0;
        if (!Mutator ||
            !Mutator->HasTokensToStartWith() ||
            !Mutator->HasLivesToStartPlayerWith())
        {
            return false;
        }

        const float Tokens =
            Mutator->TokensToStartWith.Evaluate(0.0f);
        const float Lives =
            Mutator->LivesToStartPlayerWith.Evaluate(0.0f);
        if (!std::isfinite(Tokens) ||
            !std::isfinite(Lives) ||
            Tokens < 0.0f ||
            Lives < 1.0f ||
            Tokens > 1000000.0f ||
            Lives > 1000.0f)
        {
            return false;
        }

        OutTokens = static_cast<int32>(
            std::round(Tokens));
        OutLives = static_cast<int32>(
            std::round(Lives));
        return OutLives > 0;
    }

    void PublishWaxPlayerData(
        AFortAthenaMutator_Wax* Mutator,
        int32 EntrySize,
        bool bMarkEveryEntry)
    {
        if (!Mutator || EntrySize <= 0)
            return;

        auto& PlayerData = Mutator->PlayerData;
        auto& Entries = PlayerData.Entries;
        if (bMarkEveryEntry)
        {
            for (int32 Index = 0;
                 Index < Entries.Num();
                 ++Index)
            {
                auto& Entry =
                    Entries.Get(Index, EntrySize);
                PlayerData.MarkItemDirty(Entry);
            }
        }
        PlayerData.MarkArrayDirty();
        Mutator->ForceNetUpdate();
    }

    bool EnsureWaxPlayerData(
        AFortAthenaMutator_Wax* Mutator,
        AFortPlayerStateAthena* RequiredPlayerState,
        const char* Reason)
    {
        auto& State = GWaxCompatibilityState;
        UWorld* World = UWorld::GetWorld();
        auto GameState =
            World && World->GameState
                ? World->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        int32 EntrySize = 0;
        if (!World || !GameState ||
            !ValidateWaxPlayerData(Mutator, EntrySize))
        {
            if (!State.bLoggedInvalidPlayerData)
            {
                State.bLoggedInvalidPlayerData = true;
                SDK::DbgLog(
                    "[Wax1040] reflected PlayerData layout "
                    "is unavailable or invalid\n");
            }
            return false;
        }

        auto& PlayerData = Mutator->PlayerData;
        auto& Entries = PlayerData.Entries;
        const bool bArrayWasEmpty = Entries.Num() == 0;
        const bool bAppendingToNativeData =
            !State.bCompatibilityOwnsPlayerData &&
            !bArrayWasEmpty;
        if (bAppendingToNativeData)
        {
            if (!State.bLoggedNativePlayerData)
            {
                State.bLoggedNativePlayerData = true;
                SDK::DbgLog(
                    "[Wax1040] native mutator owns PlayerData "
                    "entries=%d; compatibility will only append "
                    "missing late players\n",
                    Entries.Num());
            }

            if (!RequiredPlayerState)
                return true;

            if (FindWaxPlayerDataEntry(
                    Mutator,
                    RequiredPlayerState,
                    EntrySize))
            {
                State.MissingPlayerDataSince.erase(
                    RequiredPlayerState);
                return true;
            }
        }

        int32 InitialTokens = 0;
        int32 InitialLives = 0;
        if (!ResolveWaxInitialValues(
                Mutator,
                InitialTokens,
                InitialLives))
        {
            return false;
        }

        std::vector<AFortPlayerStateAthena*> Players;
        auto AddCandidate =
            [&](AFortPlayerStateAthena* Candidate)
            {
                if (!IsLiveObject(Candidate) ||
                    (Candidate->HasbIsSpectator() &&
                     Candidate->bIsSpectator) ||
                    std::find(
                        Players.begin(),
                        Players.end(),
                        Candidate) != Players.end())
                {
                    return;
                }
                Players.push_back(Candidate);
            };

        AddCandidate(RequiredPlayerState);
        if (!bAppendingToNativeData &&
            GameState->HasPlayerArray())
        {
            auto& PlayerArray = GameState->PlayerArray;
            if (IsSaneArray(
                    PlayerArray.Num(),
                    PlayerArray.Max(),
                    256) &&
                IsReadableArrayStorage(
                    PlayerArray.Data,
                    PlayerArray.Num(),
                    sizeof(AFortPlayerStateAthena*)))
            {
                for (auto PlayerState : PlayerArray)
                    AddCandidate(PlayerState);
            }
        }

        int32 AddedEntries = 0;
        for (auto PlayerState : Players)
        {
            if (FindWaxPlayerDataEntry(
                    Mutator,
                    PlayerState,
                    EntrySize))
            {
                continue;
            }

            auto NewEntry =
                static_cast<FWaxPlayerDataEntry*>(
                    FMemory::Malloc(EntrySize));
            if (!NewEntry)
                continue;

            int32 InitialTeamTokens = InitialTokens;
            if (bAppendingToNativeData &&
                PlayerState->HasTeamIndex())
            {
                // Existing native rows already carry the authoritative team
                // total. Seed only the appended row from that snapshot; do
                // not rewrite native Current/Previous history.
                for (int32 EntryIndex = 0;
                     EntryIndex < Entries.Num();
                     ++EntryIndex)
                {
                    auto& Existing =
                        Entries.Get(EntryIndex, EntrySize);
                    if (IsLiveObject(Existing.PlayerState) &&
                        Existing.PlayerState->HasTeamIndex() &&
                        Existing.PlayerState->TeamIndex ==
                            PlayerState->TeamIndex)
                    {
                        InitialTeamTokens =
                            Existing.CurrentTeamTokens +
                            InitialTokens;
                        break;
                    }
                }
            }

            memset(NewEntry, 0, EntrySize);
            NewEntry->ReplicationID = -1;
            NewEntry->ReplicationKey = -1;
            NewEntry->MostRecentArrayReplicationKey = -1;
            NewEntry->PlayerState = PlayerState;
            NewEntry->bPermanentlyWaxed = false;
            NewEntry->bPlayerWasLeader = false;
            NewEntry->TokenBasedPlacement = 0;
            NewEntry->CurrentTokens = InitialTokens;
            NewEntry->PreviousTokens = InitialTokens;
            NewEntry->CurrentTeamTokens =
                InitialTeamTokens;
            NewEntry->PreviousTeamTokens =
                InitialTeamTokens;
            NewEntry->CurrentKills = 0;
            NewEntry->PreviousKills = 0;
            NewEntry->CurrentLives = InitialLives;
            NewEntry->PreviousVictimLocation = FVector{};

            auto& StoredEntry =
                Entries.Add(*NewEntry, EntrySize);
            FMemory::Free(NewEntry);
            PlayerData.MarkItemDirty(StoredEntry);
            State.MissingPlayerDataSince.erase(
                PlayerState);
            ++AddedEntries;
        }

        // Ownership is claimed only after a successful mutation of an empty
        // array. A failed scalable-float read/allocation must leave a later
        // native initialization authoritative.
        if (AddedEntries > 0)
        {
            if (!State.bCompatibilityOwnsPlayerData &&
                bArrayWasEmpty)
            {
                State.bCompatibilityOwnsPlayerData = true;
            }
            if (PlayerData.OwningMutator != Mutator)
                PlayerData.OwningMutator = Mutator;
        }

        // Only a fully compatibility-owned array is safe to normalize as a
        // whole. Append-only repair must preserve every native row's previous
        // score so the stock HUD can animate the native transition history.
        bool bTeamScoreChanged = false;
        if (State.bCompatibilityOwnsPlayerData)
        {
            for (int32 EntryIndex = 0;
                 EntryIndex < Entries.Num();
                 ++EntryIndex)
            {
                auto& Entry =
                    Entries.Get(EntryIndex, EntrySize);
                if (!IsLiveObject(Entry.PlayerState) ||
                    !Entry.PlayerState->HasTeamIndex())
                {
                    continue;
                }

                int32 TeamTokens = 0;
                for (int32 OtherIndex = 0;
                     OtherIndex < Entries.Num();
                     ++OtherIndex)
                {
                    auto& Other =
                        Entries.Get(OtherIndex, EntrySize);
                    if (IsLiveObject(Other.PlayerState) &&
                        Other.PlayerState->HasTeamIndex() &&
                        Other.PlayerState->TeamIndex ==
                            Entry.PlayerState->TeamIndex)
                    {
                        TeamTokens += Other.CurrentTokens;
                    }
                }

                if (Entry.CurrentTeamTokens != TeamTokens ||
                    Entry.PreviousTeamTokens != TeamTokens)
                {
                    Entry.CurrentTeamTokens = TeamTokens;
                    Entry.PreviousTeamTokens = TeamTokens;
                    PlayerData.MarkItemDirty(Entry);
                    bTeamScoreChanged = true;
                }
            }
        }

        if (AddedEntries > 0 || bTeamScoreChanged)
        {
            PlayerData.MarkArrayDirty();
            Mutator->ForceNetUpdate();
            SDK::DbgLog(
                "[Wax1040] %s PlayerData "
                "reason=%s added=%d total=%d tokens=%d lives=%d\n",
                State.bCompatibilityOwnsPlayerData
                    ? "initialized fallback"
                    : "appended missing native",
                Reason ? Reason : "unknown",
                AddedEntries,
                Entries.Num(),
                InitialTokens,
                InitialLives);
        }
        State.bLoggedInvalidPlayerData = false;
        return RequiredPlayerState == nullptr ||
            FindWaxPlayerDataEntry(
                Mutator,
                RequiredPlayerState,
                EntrySize) != nullptr;
    }

    void TickWaxCompatibility(
        AFortGameStateAthena* GameState,
        double Now)
    {
        auto& State = GWaxCompatibilityState;
        if (!IsCurrentWaxGameState(GameState) ||
            !IsWaxGameplayPhase(GameState) ||
            !GNativeLTMCompatibilityState.bCompatibilityComplete ||
            Now < State.NextPlayerDataReconcileTime)
        {
            return;
        }
        State.NextPlayerDataReconcileTime = Now + 0.50;

        auto Mutator = ResolveWaxMutator(GameState);
        if (!Mutator)
            return;
        if (State.CompatibilityCompletedObservedTime < 0.0)
            State.CompatibilityCompletedObservedTime = Now;

        int32 EntrySize = 0;
        if (!ValidateWaxPlayerData(Mutator, EntrySize))
        {
            if (!State.bLoggedInvalidPlayerData)
            {
                State.bLoggedInvalidPlayerData = true;
                SDK::DbgLog(
                    "[Wax1040] waiting for valid PlayerData "
                    "reflection\n");
            }
            return;
        }

        auto& Entries = Mutator->PlayerData.Entries;
        if (Entries.Num() > 0 &&
            !State.bCompatibilityOwnsPlayerData)
        {
            if (!State.bLoggedNativePlayerData)
            {
                State.bLoggedNativePlayerData = true;
                SDK::DbgLog(
                    "[Wax1040] native PlayerData became ready "
                    "entries=%d\n",
                    Entries.Num());
            }
        }

        if (State.bCompatibilityOwnsPlayerData)
        {
            EnsureWaxPlayerData(
                Mutator,
                nullptr,
                "watchdog-owned");
            return;
        }

        if (Entries.Num() == 0)
        {
            const double FallbackGraceStart =
                (std::max)(
                    State.MutatorResolvedTime,
                    State.CompatibilityCompletedObservedTime);
            if (FallbackGraceStart < 0.0 ||
                Now < FallbackGraceStart +
                        WaxPlayerDataGraceSeconds)
            {
                return;
            }

            EnsureWaxPlayerData(
                Mutator,
                nullptr,
                "watchdog-empty");
            return;
        }

        // A non-empty array remains native-owned. Reconcile only individual
        // players that stay absent past their own grace window; never clear or
        // reinitialize native entries.
        std::unordered_set<AFortPlayerStateAthena*> LivePlayers;
        bool bReadPlayerArray = false;
        if (GameState->HasPlayerArray())
        {
            auto& PlayerArray = GameState->PlayerArray;
            if (IsSaneArray(
                    PlayerArray.Num(),
                    PlayerArray.Max(),
                    256) &&
                IsReadableArrayStorage(
                    PlayerArray.Data,
                    PlayerArray.Num(),
                    sizeof(AFortPlayerStateAthena*)))
            {
                bReadPlayerArray = true;
                for (auto PlayerState : PlayerArray)
                {
                    if (!IsLiveObject(PlayerState) ||
                        (PlayerState->HasbIsSpectator() &&
                         PlayerState->bIsSpectator))
                    {
                        continue;
                    }

                    LivePlayers.insert(PlayerState);
                    if (FindWaxPlayerDataEntry(
                            Mutator,
                            PlayerState,
                            EntrySize))
                    {
                        State.MissingPlayerDataSince.erase(
                            PlayerState);
                        continue;
                    }

                    if (WaxPlayerDataGraceElapsed(
                            PlayerState, Now))
                    {
                        EnsureWaxPlayerData(
                            Mutator,
                            PlayerState,
                            "watchdog-late-player");
                    }
                }
            }
        }

        if (bReadPlayerArray)
        {
            for (auto Iterator =
                     State.MissingPlayerDataSince.begin();
                 Iterator !=
                     State.MissingPlayerDataSince.end();)
            {
                if (!IsLiveObject(Iterator->first) ||
                    !LivePlayers.contains(Iterator->first))
                {
                    Iterator =
                        State.MissingPlayerDataSince.erase(
                            Iterator);
                }
                else
                {
                    ++Iterator;
                }
            }
        }
    }
}

void FFortAthenaNativeLTMCompatibility::
    HandleAshtonPlayerReady(
        AFortPlayerControllerAthena* PlayerController)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState
                  ->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsLiveObject(GameState) ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeAshtonDescriptor(
            GNativeLTMCompatibilityState.Descriptor))
    {
        return;
    }

    auto& State = GAuthoredNativeLTMPhaseState;
    auto ReadyPawn =
        IsLiveObject(PlayerController) &&
                PlayerController->HasMyFortPawn()
            ? PlayerController->MyFortPawn
            : nullptr;
    auto PreviousReadyPawn =
        State.AshtonReadyPawns.find(
            PlayerController);
    const bool bNewPawn =
        IsLiveObject(ReadyPawn) &&
        (PreviousReadyPawn ==
             State.AshtonReadyPawns.end() ||
         PreviousReadyPawn->second != ReadyPawn);
    if (bNewPawn)
    {
        State.AshtonReadyPawns[
            PlayerController] = ReadyPawn;
        State.AshtonInitializedVillainItems.erase(
            PlayerController);
        State.AshtonInitializedLeaderItems.erase(
            PlayerController);
        State.AshtonInitializedLeaderBackingItems.erase(
            PlayerController);
        State.AshtonMiloChildrenNotBefore.erase(
            PlayerController);
        State.AshtonMiloRecoveryNotBefore.erase(
            PlayerController);
        State.AshtonMiloResetAttempted.erase(
            PlayerController);
        State.AshtonMiloFallbackAttempted.erase(
            PlayerController);
        State.AshtonFocusedMiloPrimaryItems.erase(
            PlayerController);
        State.AshtonMiloHealthReconcileStates.erase(
            PlayerController);
        State.AshtonMiloQuickbarReconcileStates.erase(
            PlayerController);
        State.AshtonCarmineHealthReconcileStates.erase(
            PlayerController);
        CancelAshtonLeaderPromotion(
            PlayerController);
    }
    State.AshtonEliminatedLeaders.erase(
        PlayerController);
    if (!EnsureAshtonVillainConfiguration(GameState))
    {
        // The pawn is still authoritative even when the authored mutator or
        // its soft definitions are a frame late. Recording it here lets the
        // watchdog reconcile as soon as configuration resolves.
        return;
    }
    ReconcileAshtonVillainPlayer(
        PlayerController, "pawn-ready");
    ReconcileAshtonStoneHealthEffects(
        GameState,
        State.AshtonMutator,
        "pawn-ready");
}

bool FFortAthenaNativeLTMCompatibility::
    IsCurrentAshtonLeader(
        AFortPlayerControllerAthena* PlayerController)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState
                  ->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsLiveObject(GameState) ||
        !IsLiveObject(PlayerController) ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeAshtonDescriptor(
            GNativeLTMCompatibilityState.Descriptor) ||
        !EnsureAshtonVillainConfiguration(GameState))
    {
        return false;
    }

    auto& State = GAuthoredNativeLTMPhaseState;
    auto PublishedLeader =
        State.AshtonMutator &&
                State.AshtonMutator
                    ->HasVillainLeaderPC()
            ? State.AshtonMutator
                  ->VillainLeaderPC
            : nullptr;
    if (!State.bAshtonLeaderVacant &&
        PublishedLeader == PlayerController)
    {
        return true;
    }
    if (!State.bAshtonLeaderVacant &&
        IsValidAshtonLeaderController(
            PublishedLeader))
    {
        // Evidence on a stale former carrier never outranks a different
        // living, published leader.
        return false;
    }
    if (State.bAshtonLeaderVacant)
        return false;

    if (State.AshtonInitializedLeaderItems.contains(
            PlayerController))
    {
        return true;
    }
    if (!IsLiveObject(PlayerController->WorldInventory))
        return false;
    return PlayerController->WorldInventory
               ->Inventory.ReplicatedEntries.Search(
                   [&](FFortItemEntry& Entry)
                   {
                       return IsAnyAshtonLeaderArtifact(
                           State.AshtonMutator,
                           Entry.ItemDefinition);
                   },
                   FFortItemEntry::Size()) != nullptr;
}

void FFortAthenaNativeLTMCompatibility::
    HandleAshtonLeaderEliminated(
        AFortPlayerControllerAthena* PlayerController,
        bool bAfterNativeDeath)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState
                  ->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsLiveObject(GameState) ||
        !IsLiveObject(PlayerController) ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeAshtonDescriptor(
            GNativeLTMCompatibilityState.Descriptor) ||
        !EnsureAshtonVillainConfiguration(GameState))
    {
        return;
    }

    auto& State = GAuthoredNativeLTMPhaseState;
    auto Ashton = State.AshtonMutator;
    if (!IsLiveObject(Ashton))
        return;

    auto PublishedLeader =
        Ashton->HasVillainLeaderPC()
            ? Ashton->VillainLeaderPC
            : nullptr;
    const bool bVictimPublished =
        PublishedLeader == PlayerController;
    const bool bVictimTracked =
        State.AshtonInitializedLeaderItems.contains(
            PlayerController);
    bool bVictimHasArtifact = false;
    if (IsLiveObject(PlayerController->WorldInventory))
    {
        bVictimHasArtifact =
            PlayerController->WorldInventory
                ->Inventory.ReplicatedEntries.Search(
                    [&](FFortItemEntry& Entry)
                    {
                        return IsAnyAshtonLeaderArtifact(
                            Ashton,
                            Entry.ItemDefinition);
                    },
                    FFortItemEntry::Size()) != nullptr;
    }
    const bool bDifferentLivingLeader =
        !State.bAshtonLeaderVacant &&
        PublishedLeader &&
        PublishedLeader != PlayerController &&
        IsValidAshtonLeaderController(
            PublishedLeader);
    const bool bWasStaged =
        State.AshtonEliminatedLeaders.contains(
            PlayerController);

    if (bDifferentLivingLeader &&
        !bWasStaged)
    {
        // A stale map entry or copied artifact on this victim must not vacate
        // and demote the actual living Thanos. During a staged Thanos death,
        // every replacement pointer is untrusted; the SelectNext hook blocks
        // synchronous selection and can run its all-stones fallback only
        // after this post-native cleanup has completed.
        const int32 StaleRowsRemoved =
            RemoveAllAshtonLeaderArtifacts(
                Ashton, PlayerController);
        SDK::DbgLog(
            "[Ashton1040] preserved authoritative leader "
            "after nonleader death victim=%p authoritative=%p "
            "rows=%d\n",
            static_cast<void*>(PlayerController),
            static_cast<void*>(PublishedLeader),
            StaleRowsRemoved);
        return;
    }
    if (!bVictimPublished &&
        !bVictimTracked &&
        !bVictimHasArtifact &&
        !bWasStaged)
    {
        return;
    }

    State.bAshtonLeaderVacant = true;
    State.AshtonEliminatedLeaders.insert(
        PlayerController);
    CancelAshtonLeaderPromotion(
        PlayerController);

    if (!bAfterNativeDeath)
    {
        State.AshtonStagedDeathVictim =
            PlayerController;
        State.AshtonAuthorizedAllStoneFallback =
            nullptr;
        // Retain the published pointer and gadget until the stock death path
        // has observed Thanos and emitted its fallen cue/message. Vacancy plus
        // the grant/pickup guards already blocks its killer-transfer/drop.
        SDK::DbgLog(
            "[Ashton1040] staged leader death "
            "victim=%p published=%d tracked=%d artifact=%d\n",
            static_cast<void*>(PlayerController),
            bVictimPublished ? 1 : 0,
            bVictimTracked ? 1 : 0,
            bVictimHasArtifact ? 1 : 0);
        return;
    }

    auto UnexpectedLeader =
        Ashton->HasVillainLeaderPC()
            ? Ashton->VillainLeaderPC
            : nullptr;
    const bool bAuthorizedAllStoneFallback =
        State.AshtonStagedDeathVictim ==
            PlayerController &&
        UnexpectedLeader &&
        UnexpectedLeader != PlayerController &&
        State.AshtonAuthorizedAllStoneFallback ==
            UnexpectedLeader &&
        CountCapturedAshtonStones(Ashton) >= 6 &&
        IsValidAshtonLeaderController(
            UnexpectedLeader);
    if (bAuthorizedAllStoneFallback)
    {
        const int32 VictimRowsRemoved =
            RemoveAllAshtonLeaderArtifacts(
                Ashton, PlayerController);
        State.AshtonAuthorizedAllStoneFallback =
            nullptr;
        State.AshtonStagedDeathVictim =
            nullptr;
        State.bAshtonLeaderVacant = false;
        ReconcileAshtonVillainPlayers(
            GameState,
            "all-stones-post-death-leader");
        SDK::DbgLog(
            "[Ashton1040] committed authorized all-stones "
            "leader after native death victim=%p leader=%p "
            "victimRows=%d\n",
            static_cast<void*>(PlayerController),
            static_cast<void*>(UnexpectedLeader),
            VictimRowsRemoved);
        return;
    }
    State.AshtonAuthorizedAllStoneFallback =
        nullptr;
    if (State.AshtonStagedDeathVictim ==
        PlayerController)
    {
        State.AshtonStagedDeathVictim =
            nullptr;
    }
    if (Ashton->HasVillainLeaderPC())
    {
        Ashton->VillainLeaderPC = nullptr;
        Ashton->ForceNetUpdate();
    }

    const int32 VictimRowsRemoved =
        RemoveAllAshtonLeaderArtifacts(
            Ashton, PlayerController);
    int32 UnexpectedRowsRemoved = 0;
    if (UnexpectedLeader &&
        UnexpectedLeader != PlayerController)
    {
        UnexpectedRowsRemoved =
            RemoveAllAshtonLeaderArtifacts(
                Ashton, UnexpectedLeader);
        ReconcileAshtonVillainPlayer(
            UnexpectedLeader,
            "leader-death-native-transfer-cleanup");
    }
    ReconcileAshtonVillainPlayers(
        GameState,
        "leader-death-vacancy-sweep");

    SDK::DbgLog(
        "[Ashton1040] cleared eliminated leader "
        "victim=%p published=%d tracked=%d artifact=%d "
        "victimRows=%d unexpectedLeader=%p unexpectedRows=%d "
        "awaitingStone=1\n",
        static_cast<void*>(PlayerController),
        bVictimPublished ? 1 : 0,
        bVictimTracked ? 1 : 0,
        bVictimHasArtifact ? 1 : 0,
        VictimRowsRemoved,
        static_cast<void*>(UnexpectedLeader),
        UnexpectedRowsRemoved);
}

bool FFortAthenaNativeLTMCompatibility::
    ShouldSuppressAshtonLeaderWorldPickup(
        const UFortItemDefinition* ItemDefinition)
{
    UWorld* World = UWorld::GetWorld();
    if (!IsSupportedBuild() ||
        !World ||
        !IsLiveObject(ItemDefinition) ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeAshtonDescriptor(
            GNativeLTMCompatibilityState.Descriptor))
    {
        return false;
    }
    return IsAnyAshtonLeaderArtifact(
        GAuthoredNativeLTMPhaseState.AshtonMutator,
        ItemDefinition);
}

bool FFortAthenaNativeLTMCompatibility::
    ShouldRejectAshtonLeaderGrant(
        AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition)
{
    if (!ShouldSuppressAshtonLeaderWorldPickup(
            ItemDefinition))
    {
        return false;
    }
    auto& State = GAuthoredNativeLTMPhaseState;
    const bool bUnauthorized =
        State.bAshtonLeaderVacant ||
        !IsLiveObject(PlayerController) ||
        !IsLiveObject(State.AshtonMutator) ||
        !State.AshtonMutator->HasVillainLeaderPC() ||
        State.AshtonMutator->VillainLeaderPC !=
            PlayerController ||
        !IsValidAshtonLeaderController(
            PlayerController);
    if (bUnauthorized)
        return true;

    // The base passive legitimately creates a distinct backing row, but a
    // second grant of either exact definition would apply the same persistent
    // gauntlet ability set again.
    return IsLiveObject(
               PlayerController->WorldInventory) &&
        PlayerController->WorldInventory
                ->Inventory.ReplicatedEntries.Search(
                    [&](FFortItemEntry& Entry)
                    {
                        return Entry.ItemDefinition ==
                            ItemDefinition;
                    },
                    FFortItemEntry::Size()) != nullptr;
}

bool FFortAthenaNativeLTMCompatibility::
    ShouldPreserveAshtonInventoryItem(
        AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState
                  ->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsLiveObject(GameState) ||
        !IsLiveObject(PlayerController) ||
        !IsLiveObject(ItemDefinition) ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeAshtonDescriptor(
            GNativeLTMCompatibilityState.Descriptor) ||
        !PlayerController->HasPlayerState() ||
        !IsLiveObject(PlayerController->PlayerState))
    {
        return false;
    }

    const bool bHasAuthoredConfiguration =
        EnsureAshtonVillainConfiguration(GameState);
    auto PlayerState =
        static_cast<AFortPlayerStateAthena*>(
            PlayerController->PlayerState);
    const uint8_t VillainTeam =
        bHasAuthoredConfiguration
            ? GAuthoredNativeLTMPhaseState
                  .AshtonVillainTeam
            : 3;
    if (!PlayerState->HasTeamIndex() ||
        PlayerState->TeamIndex != VillainTeam)
    {
        return false;
    }

    auto Ashton =
        bHasAuthoredConfiguration
            ? GAuthoredNativeLTMPhaseState
                  .AshtonMutator
            : nullptr;
    const bool bIsLeader =
        Ashton &&
        !GAuthoredNativeLTMPhaseState
             .bAshtonLeaderVacant &&
        Ashton->HasVillainLeaderPC() &&
        Ashton->VillainLeaderPC ==
            PlayerController &&
        IsValidAshtonLeaderController(
            PlayerController);
    if (bIsLeader &&
        IsAshtonLeaderInventoryDefinition(
            Ashton, ItemDefinition))
    {
        // The Ashton gadget entry owns the transformation while its backing
        // weapon definition can appear as the equipped inventory artifact.
        // Both belong exclusively to the one current Thanos.
        return true;
    }

    if (IsAnyAshtonLeaderArtifact(
            Ashton, ItemDefinition))
    {
        return false;
    }

    if (!bIsLeader &&
        IsAshtonVillainUtilityDefinition(
            ItemDefinition))
    {
        // Chitauri retain their harvesting/build/edit tools and authored
        // material drip. These utility rows are not ordinary loot.
        return true;
    }

    if (!bIsLeader &&
        bHasAuthoredConfiguration &&
        GAuthoredNativeLTMPhaseState
            .AshtonVillainGear.contains(
                ItemDefinition))
    {
        return true;
    }

    // InventoryOverride can resolve a frame after the spawn-island inventory
    // cleanup starts. Preserve the exact authored Chitauri definitions during
    // that window so the aircraft transition never strips their original
    // gadget instance/GUID before the normal reconciliation takes over.
    const auto Name = ItemDefinition->Name.ToWString();
    static constexpr const wchar_t*
        ChitauriItemNames[] = {
            L"AGID_JetPack_AshtonPack_Milo",
            L"AGID_AshtonPack_Milo",
            L"WID_AshtonPack_Milo",
            L"WID_AshtonPack_Milo_Launcher"
        };
    for (const auto ChitauriItemName :
         ChitauriItemNames)
    {
        if (Name == ChitauriItemName)
            return true;
    }
    return false;
}

bool FFortAthenaNativeLTMCompatibility::
    ShouldBlockAshtonInventoryDrop(
        AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition)
{
    return ShouldPreserveAshtonInventoryItem(
        PlayerController, ItemDefinition);
}

bool FFortAthenaNativeLTMCompatibility::
    ShouldRejectAshtonPickup(
        AFortPlayerPawnAthena* Pawn,
        const UFortItemDefinition* ItemDefinition)
{
    if (!IsLiveObject(Pawn) ||
        !IsLiveObject(ItemDefinition))
    {
        return false;
    }

    auto PlayerController =
        Pawn->HasController() &&
                IsLiveObject(Pawn->Controller)
            ? Pawn->Controller
                  ->Cast<AFortPlayerControllerAthena>()
            : nullptr;
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState
                  ->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsLiveObject(GameState) ||
        !IsLiveObject(PlayerController) ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeAshtonDescriptor(
            GNativeLTMCompatibilityState.Descriptor) ||
        !PlayerController->HasPlayerState() ||
        !IsLiveObject(PlayerController->PlayerState))
    {
        return false;
    }

    const bool bHasAuthoredConfiguration =
        EnsureAshtonVillainConfiguration(GameState);
    auto PlayerState =
        static_cast<AFortPlayerStateAthena*>(
            PlayerController->PlayerState);
    const uint8_t VillainTeam =
        bHasAuthoredConfiguration
            ? GAuthoredNativeLTMPhaseState
                  .AshtonVillainTeam
            : 3;
    const bool bIsStone =
        IsAshtonStoneItemDefinition(ItemDefinition);
    const bool bIsLeaderArtifact =
        IsAnyAshtonLeaderArtifact(
            GAuthoredNativeLTMPhaseState
                .AshtonMutator,
            ItemDefinition);
    if (!PlayerState->HasTeamIndex() ||
        PlayerState->TeamIndex != VillainTeam)
    {
        // Infinity Stones belong exclusively to the Chitauri/villain team.
        // Heroes retain their normal authored loot rules, but every server
        // pickup route must reject a stone before native capture can run.
        return bIsStone || bIsLeaderArtifact;
    }

    auto& AshtonState =
        GAuthoredNativeLTMPhaseState;
    auto LeaderDefinition =
        AshtonState.AshtonMutator &&
                AshtonState.AshtonMutator
                    ->HasVillainLeaderItemDef()
            ? AshtonState.AshtonMutator
                  ->VillainLeaderItemDef
            : nullptr;
    const bool bIsLeader =
        bHasAuthoredConfiguration &&
        AshtonState.AshtonMutator &&
        !AshtonState.bAshtonLeaderVacant &&
        AshtonState.AshtonMutator
            ->HasVillainLeaderPC() &&
        AshtonState.AshtonMutator
                ->VillainLeaderPC ==
            PlayerController &&
        IsValidAshtonLeaderController(
            PlayerController);
    if ((bIsLeader &&
         IsAshtonLeaderInventoryDefinition(
             AshtonState.AshtonMutator,
             ItemDefinition)) ||
        (!bIsLeader &&
         ItemDefinition != LeaderDefinition &&
         ShouldPreserveAshtonInventoryItem(
             PlayerController, ItemDefinition)))
    {
        return false;
    }

    // The six game-mode stones are captured through their long-use pickup
    // actors, not added as ordinary villain loot. Keep that authored path
    // eligible while rejecting all normal weapons, ammo and consumables.
    if (bIsStone)
        return false;
    return true;
}

bool FFortAthenaNativeLTMCompatibility::
    ShouldBlockAshtonGenericPickup(
        AFortPlayerPawnAthena* Pawn,
        const UFortItemDefinition* ItemDefinition)
{
    // Ashton stones are five-second game-mode Use objectives. No team may
    // route them through the generic pickup/spline/inventory pipeline; villain
    // admission remains in ServerAttemptInteract so the authored montage,
    // gameplay tags and OnPickupDestroying callback all run.
    return IsCurrentAshtonStone(
        Pawn, ItemDefinition);
}

bool FFortAthenaNativeLTMCompatibility::
    IsCurrentAshtonStone(
        AFortPlayerPawnAthena* Pawn,
        const UFortItemDefinition* ItemDefinition)
{
    (void)Pawn;
    UWorld* World = nullptr;
    AFortGameStateAthena* GameState = nullptr;
    AFortAthenaMutator_Ashton* Ashton = nullptr;
    int32 StoneType = -1;
    // Generic inventory/spline routes can arrive during a possession race
    // with no live pawn. The current playlist, mutator and exact definition
    // are sufficient to identify an objective, so never let that null pawn
    // turn into a generic-pickup bypass.
    return ResolveCurrentAshtonStoneRuntime(
            ItemDefinition,
            World,
            GameState,
            Ashton,
            StoneType);
}

bool FFortAthenaNativeLTMCompatibility::
    IsAshtonStoneCaptured(
        const UFortItemDefinition* ItemDefinition)
{
    UWorld* World = nullptr;
    AFortGameStateAthena* GameState = nullptr;
    AFortAthenaMutator_Ashton* Ashton = nullptr;
    int32 StoneType = -1;
    if (!ResolveCurrentAshtonStoneRuntime(
            ItemDefinition,
            World,
            GameState,
            Ashton,
            StoneType))
    {
        return false;
    }

    if (GAuthoredNativeLTMPhaseState
            .bAshtonStoneCaptureLocked[
                StoneType])
    {
        return true;
    }
    auto Stone =
        FindAshtonStoneState(
            Ashton, StoneType);
    return Stone &&
        Stone->StoneState ==
            AshtonStoneStateCaptured;
}

bool FFortAthenaNativeLTMCompatibility::
    TryCompleteAshtonStonePickup(
        AFortPlayerPawnAthena* Pawn,
        AFortPickupAthena* Pickup,
        const UFortItemDefinition* ItemDefinition,
        const char* Reason)
{
    UWorld* World = nullptr;
    AFortGameStateAthena* GameState = nullptr;
    AFortAthenaMutator_Ashton* Ashton = nullptr;
    int32 StoneType = -1;
    if (!ResolveCurrentAshtonStoneRuntime(
            ItemDefinition,
            World,
            GameState,
            Ashton,
            StoneType))
    {
        return false;
    }

    // From this point the item is an active Ashton objective and must never
    // enter generic inventory handling. A non-villain or malformed collector
    // consumes only the request; it does not consume the world objective.
    AFortPlayerControllerAthena* Collector = nullptr;
    if (!ResolveAshtonVillainCollector(
            Pawn, Collector))
    {
        SDK::DbgLog(
            "[Ashton1040] rejected stone completion "
            "collectorPawn=%p item=%s reason=%s\n",
            static_cast<void*>(Pawn),
            ItemDefinition->Name
                .ToString().c_str(),
            Reason ? Reason : "unknown");
        return true;
    }

    if (IsLiveObject(Pickup) &&
        !Pickup->HasAuthority())
    {
        return true;
    }

    auto Stone =
        FindAshtonStoneState(
            Ashton, StoneType);
    if (!Stone)
        return true;

    auto& PhaseState =
        GAuthoredNativeLTMPhaseState;
    const bool bAlreadyCaptured =
        PhaseState
            .bAshtonStoneCaptureLocked[
                StoneType];
    if (bAlreadyCaptured)
    {
        const int32 RemovedStoneRows =
            RemoveAshtonInventoryRows(
                Collector, nullptr, true);
        auto DuplicatePickup =
            IsLiveObject(Pickup)
                ? Pickup
                : FindActiveAshtonStonePickup(
                      World, StoneType, Pawn);
        RetireDuplicateAshtonPickup(
            DuplicatePickup);
        SDK::DbgLog(
            "[Ashton1040] retired already-captured "
            "stone without leader promotion type=%d "
            "collector=%p pickup=%p removedRows=%d "
            "reason=%s\n",
            StoneType,
            static_cast<void*>(Collector),
            static_cast<void*>(DuplicatePickup),
            RemovedStoneRows,
            Reason ? Reason : "unknown");
        return true;
    }

    const int32 CapturedBefore =
        CountCapturedAshtonStones(Ashton);
    auto LeaderBefore =
        Ashton->HasVillainLeaderPC() &&
                !PhaseState.bAshtonLeaderVacant &&
                IsValidAshtonLeaderController(
                    Ashton->VillainLeaderPC)
            ? Ashton->VillainLeaderPC
            : nullptr;

    auto ObjectivePickup =
        IsLiveObject(Pickup) &&
                GetAshtonStoneType(
                    Pickup
                        ->PrimaryPickupItemEntry
                        .ItemDefinition) ==
                    StoneType
            ? Pickup
            : FindActiveAshtonStonePickup(
                  World, StoneType, Pawn);
    if (IsLiveObject(ObjectivePickup))
    {
        ObjectivePickup->PickupLocationData
            .PickupTarget = Pawn;
    }

    bool bDispatchedDestroy = false;
    UClass* ExactPickupClass =
        ResolveAshtonRockPickupClass();
    UFunction* NativeDestroyFunction =
        Ashton->GetFunction(
            "OnPickupDestroying");
    const bool bCanDispatchNativeDestroy =
        !PhaseState
             .bAshtonStoneCaptureLocked[
                 StoneType] &&
        IsLiveObject(ObjectivePickup) &&
        ExactPickupClass &&
        ObjectivePickup->IsA(
            ExactPickupClass) &&
        NativeDestroyFunction &&
        !PhaseState
             .bAshtonStoneDestroyDispatchInFlight[
                 StoneType];
    if (bCanDispatchNativeDestroy)
    {
        // StoneState can be latched Captured before a stripped/delayed
        // OnPickupDestroying delegate runs. The external completion lock—not
        // that replicated enum—is the proof that the authored gem tags,
        // pickup montage and leader effects have already been dispatched.
        bool& DispatchInFlight =
            PhaseState
                .bAshtonStoneDestroyDispatchInFlight[
                    StoneType];
        struct FScopedDispatchReset
        {
            bool& Flag;
            ~FScopedDispatchReset()
            {
                Flag = false;
            }
        };
        DispatchInFlight = true;
        FScopedDispatchReset DispatchReset{
            DispatchInFlight};
        auto DispatchPickup =
            ObjectivePickup;
        auto DispatchDefinition =
            const_cast<UFortItemDefinition*>(
                ItemDefinition);
        // Dispatch the verified Ashton listener directly. The generic
        // multicast helper blindly dereferences every weak listener and can
        // crash on an unrelated stale binding; the authored listener is the
        // only target needed to apply gem tags, montage/effects and state.
        Ashton->Call<void>(
            NativeDestroyFunction,
            DispatchPickup,
            DispatchDefinition);
        bDispatchedDestroy = true;
        Stone =
            FindAshtonStoneState(
                Ashton, StoneType);
    }

    if (!Stone ||
        Stone->StoneState !=
            AshtonStoneStateCaptured)
    {
        MarkAshtonStoneCapturedFallback(
            Ashton, StoneType);
        Stone =
            FindAshtonStoneState(
                Ashton, StoneType);
    }

    const bool bCaptured =
        Stone &&
        Stone->StoneState ==
            AshtonStoneStateCaptured;
    if (!bCaptured)
    {
        SDK::DbgLog(
            "[Ashton1040] stone completion made no "
            "state progress type=%d item=%s "
            "destroyDispatched=%d reason=%s\n",
            StoneType,
            ItemDefinition->Name
                .ToString().c_str(),
            bDispatchedDestroy ? 1 : 0,
            Reason ? Reason : "unknown");
        return true;
    }

    // This external lock is written only from a proven completion route. It
    // prevents a delayed OnPickupSpawnedAndReady/timer callback from reopening
    // an already captured row before the next replication pass.
    PhaseState
        .bAshtonStoneCaptureLocked[
            StoneType] = true;
    const int32 RemovedStoneRows =
        RemoveAshtonInventoryRows(
            Collector, nullptr, true);
    FinalizeAshtonStoneLeader(
        Ashton,
        Collector,
        LeaderBefore,
        Reason);

    if (IsLiveObject(ObjectivePickup))
    {
        RetireAshtonStonePickup(
            ObjectivePickup, Pawn);
    }
    if (IsLiveObject(Pickup) &&
        Pickup != ObjectivePickup)
    {
        RetireAshtonStonePickup(
            Pickup, Pawn);
    }

    SDK::DbgLog(
        "[Ashton1040] completed stone capture "
        "type=%d collector=%p captured=%d->%d "
        "destroyDispatched=%d removedInventoryRows=%d "
        "leaderBefore=%p leaderAfter=%p reason=%s\n",
        StoneType,
        static_cast<void*>(Collector),
        CapturedBefore,
        CountCapturedAshtonStones(Ashton),
        bDispatchedDestroy ? 1 : 0,
        RemovedStoneRows,
        static_cast<void*>(LeaderBefore),
        static_cast<void*>(
            Ashton->HasVillainLeaderPC()
                ? Ashton->VillainLeaderPC
                : nullptr),
        Reason ? Reason : "unknown");
    return true;
}

bool FFortAthenaNativeLTMCompatibility::
    TryCollectWaxPickup(
        AFortPlayerPawnAthena* Pawn,
        AFortPickupAthena* Pickup)
{
    const UClass* WaxPickupClass =
        AFortGameModePickup_Wax::StaticClass();
    if (!Pickup || !WaxPickupClass ||
        !Pickup->IsA(WaxPickupClass))
    {
        return false;
    }

    // From here on the actor is a game-mode token and must never fall through
    // to generic inventory pickup handling, even if a validation gate rejects
    // this particular request.
    auto WaxPickup =
        static_cast<AFortGameModePickup_Wax*>(Pickup);
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsCurrentWaxGameState(GameState) ||
        !IsWaxGameplayPhase(GameState) ||
        !IsPlayableWaxPawn(Pawn) ||
        !Pickup->HasAuthority() ||
        (Pickup->HasbActorIsBeingDestroyed() &&
         Pickup->bActorIsBeingDestroyed) ||
        !Pickup->HasbPickedUp() ||
        Pickup->bPickedUp ||
        !WaxPickup->HasAmountOfTokens() ||
        AFortGameModePickup_Wax::AmountOfTokens__Offset !=
            0x564)
    {
        return true;
    }

    auto PlayerState =
        ResolveWaxPawnPlayerState(Pawn);
    auto PlayerController =
        Pawn->HasController() &&
                IsLiveObject(Pawn->Controller)
            ? Pawn->Controller
                  ->Cast<AFortPlayerControllerAthena>()
            : nullptr;
    if (!IsLiveObject(PlayerState) ||
        !IsLiveObject(PlayerController) ||
        PlayerController->PlayerState != PlayerState ||
        !PlayerState->HasTeamIndex() ||
        PlayerState->TeamIndex == 0 ||
        PlayerState->TeamIndex == 255)
    {
        return true;
    }

    constexpr double MaximumCollectionDistance = 800.0;
    const FVector Delta =
        Pawn->K2_GetActorLocation() -
        Pickup->K2_GetActorLocation();
    if (Delta.SizeSquared() >
        MaximumCollectionDistance *
            MaximumCollectionDistance)
    {
        SDK::DbgLog(
            "[Wax1040] rejected distant token request "
            "pawn=%p pickup=%p distanceSquared=%.1f\n",
            static_cast<void*>(Pawn),
            static_cast<void*>(Pickup),
            Delta.SizeSquared());
        return true;
    }

    const int32 Amount = WaxPickup->AmountOfTokens;
    if (Amount <= 0 || Amount > 1000000)
    {
        SDK::DbgLog(
            "[Wax1040] rejected invalid token amount=%d "
            "pickup=%p\n",
            Amount,
            static_cast<void*>(Pickup));
        return true;
    }

    auto Mutator = ResolveWaxMutator(GameState);
    if (!Mutator ||
        !EnsureWaxPlayerData(
            Mutator,
            PlayerState,
            "token-collection"))
    {
        return true;
    }

    int32 EntrySize = 0;
    if (!ValidateWaxPlayerData(Mutator, EntrySize))
        return true;
    auto Before = FindWaxPlayerDataEntry(
        Mutator, PlayerState, EntrySize);
    if (!Before ||
        Before->CurrentTokens < 0 ||
        Before->CurrentTokens >
            (std::numeric_limits<int32>::max)() - Amount)
    {
        return true;
    }

    auto CollectTokens = ResolveWaxCollectTokensNative();
    if (!CollectTokens)
        return true;

    const int32 PreviousTokens = Before->CurrentTokens;
    const int32 PreviousTeamTokens =
        Before->CurrentTeamTokens;
    bool bWasFirstGeneration =
        WaxPickup->HasbIsFirstGeneration()
            ? WaxPickup->bIsFirstGeneration
            : false;

    // Claim before entering the native scorer so duplicate RPC/overlap routes
    // cannot apply the same actor twice on the game thread.
    Pickup->bPickedUp = true;
    if (WaxPickup->HasbIsFirstGeneration())
        WaxPickup->bIsFirstGeneration = false;

    CollectTokens(Mutator, Pawn, Amount);

    int32 PostEntrySize = 0;
    auto After =
        ValidateWaxPlayerData(Mutator, PostEntrySize)
            ? FindWaxPlayerDataEntry(
                  Mutator,
                  PlayerState,
                  PostEntrySize)
            : nullptr;
    const bool bMatchEnded =
        GameState->HasGamePhase() &&
        GameState->GamePhase >=
            static_cast<uint8>(
                EAthenaGamePhase::EndGame);
    if ((!After ||
         After->CurrentTokens <= PreviousTokens) &&
        !bMatchEnded)
    {
        // The resolved routine is expected to own every score side effect.
        // If its postcondition fails, leave the actor collectible so a later
        // compatibility attempt can retry rather than silently eating it.
        Pickup->bPickedUp = false;
        if (WaxPickup->HasbIsFirstGeneration())
            WaxPickup->bIsFirstGeneration =
                bWasFirstGeneration;
        SDK::DbgLog(
            "[Wax1040] native token scorer made no progress "
            "pickup=%p player=%p before=%d amount=%d\n",
            static_cast<void*>(Pickup),
            static_cast<void*>(PlayerState),
            PreviousTokens,
            Amount);
        return true;
    }

    Pickup->FlushNetDormancy();
    Pickup->OnRep_bPickedUp();
    Pickup->ForceNetUpdate();
    Pickup->SetLifeSpan(0.10f);

    SDK::DbgLog(
        "[Wax1040] collected token pickup=%p player=%p "
        "amount=%d playerTokens=%d->%d teamTokens=%d->%d\n",
        static_cast<void*>(Pickup),
        static_cast<void*>(PlayerState),
        Amount,
        PreviousTokens,
        After ? After->CurrentTokens : PreviousTokens + Amount,
        PreviousTeamTokens,
        After ? After->CurrentTeamTokens :
                PreviousTeamTokens + Amount);
    return true;
}

bool FFortAthenaNativeLTMCompatibility::
    ShouldSuppressArsenalWorldLoot()
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;

    return World && IsCurrentArsenalGameState(GameState);
}

bool FFortAthenaNativeLTMCompatibility::
    TryClaimArsenalElimination(
        AFortPlayerStateAthena* VictimPlayerState,
        AFortPlayerPawnAthena* VictimPawn)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;

    // This helper gates a generic kill-credit block, so every non-Arsenal or
    // uncertain case must preserve the existing behavior.
    if (!World ||
        !IsCurrentArsenalGameState(GameState) ||
        !IsArsenalGameplayPhase(GameState, true) ||
        !IsLiveObject(VictimPlayerState) ||
        !IsLiveObject(VictimPawn) ||
        VictimPawn->IsDBNO())
    {
        return true;
    }

    auto& State = GArsenalCompatibilityState;
    if (State.World != World)
    {
        State = {};
        State.World = World;
    }

    auto Existing =
        State.ClaimedEliminationPawns.find(
            VictimPlayerState);
    if (Existing !=
            State.ClaimedEliminationPawns.end() &&
        Existing->second == VictimPawn)
    {
        SDK::DbgLog(
            "[Arsenal1040] suppressed duplicate elimination "
            "before kill credit victimPS=%p victimPawn=%p\n",
            static_cast<void*>(VictimPlayerState),
            static_cast<void*>(VictimPawn));
        return false;
    }

    State.ClaimedEliminationPawns[VictimPlayerState] =
        VictimPawn;
    return true;
}

void FFortAthenaNativeLTMCompatibility::
    HandleArsenalPlayerReady(
        AFortPlayerControllerAthena* PlayerController)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!World ||
        !IsCurrentArsenalGameState(GameState) ||
        !IsArsenalGameplayPhase(GameState, true) ||
        !IsLiveObject(PlayerController) ||
        !PlayerController->HasPlayerState() ||
        !IsLiveObject(PlayerController->PlayerState))
    {
        return;
    }

    auto PlayerState =
        static_cast<AFortPlayerStateAthena*>(
            PlayerController->PlayerState);

    // A ready/possession transition begins a new playable life. Clearing the
    // victim claim here handles both a genuinely new pawn and legacy respawn
    // paths that reuse the same actor address.
    if (GArsenalCompatibilityState.World == World)
        GArsenalCompatibilityState
            .ClaimedEliminationPawns.erase(PlayerState);

    ReconcileArsenalPlayerTier(
        PlayerController,
        GetArsenalKillScore(PlayerState),
        "pawn-ready");
}

void FFortAthenaNativeLTMCompatibility::
    HandleArsenalElimination(
        AFortPlayerControllerAthena* KillerController,
        AFortPlayerStateAthena* KillerPlayerState,
        AFortPlayerControllerAthena* VictimController,
        AFortPlayerStateAthena* VictimPlayerState,
        AFortPlayerPawnAthena* VictimPawn)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!World ||
        !IsCurrentArsenalGameState(GameState) ||
        !IsArsenalGameplayPhase(GameState, true) ||
        !IsLiveObject(KillerController) ||
        !IsLiveObject(KillerPlayerState) ||
        !IsLiveObject(VictimController) ||
        !IsLiveObject(VictimPlayerState) ||
        !IsLiveObject(VictimPawn) ||
        KillerController == VictimController ||
        KillerPlayerState == VictimPlayerState ||
        KillerController->PlayerState !=
            KillerPlayerState ||
        VictimController->PlayerState !=
            VictimPlayerState ||
        VictimPawn->IsDBNO())
    {
        return;
    }

    auto& State = GArsenalCompatibilityState;
    if (State.World != World)
    {
        State = {};
        State.World = World;
    }

    const int32 EffectiveScore =
        GetArsenalKillScore(KillerPlayerState);
    ReconcileArsenalPlayerTier(
        KillerController,
        EffectiveScore,
        "elimination");
}

bool FFortAthenaNativeLTMCompatibility::
    TryGetFoodFightRespawnAllowed(
        const AFortPlayerStateAthena* PlayerState,
        bool& OutAllowed)
{
    UWorld* World = UWorld::GetWorld();
    auto& Arena = GDeepFriedArenaState;
    if (!IsSupportedBuild() ||
        !World ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeFoodFightDescriptor(
            GNativeLTMCompatibilityState.Descriptor) ||
        Arena.World != World ||
        !Arena.bBindingsComplete ||
        !IsLiveObject(Arena.Barrier) ||
        !IsLiveObject(
            const_cast<AFortPlayerStateAthena*>(
                PlayerState)) ||
        !PlayerState->HasTeamIndex() ||
        !Arena.Barrier->HasTeam_0_State() ||
        !Arena.Barrier->HasTeam_1_State() ||
        !FBarrierTeamState::HasbRespawnEnabled())
    {
        return false;
    }

    int32 TeamIndex = -1;
    if (PlayerState->TeamIndex == DeepFriedBurgerTeam)
        TeamIndex = 0;
    else if (PlayerState->TeamIndex ==
             DeepFriedTomatoTeam)
        TeamIndex = 1;
    else
        return false;

    const auto& TeamState =
        TeamIndex == 0
            ? Arena.Barrier->Team_0_State
            : Arena.Barrier->Team_1_State;
    OutAllowed =
        TeamState.bRespawnEnabled &&
        !Arena.bObjectiveDestroyed[TeamIndex];
    return true;
}

bool FFortAthenaNativeLTMCompatibility::
    TryGetWaxRespawnAllowed(
        const AFortPlayerStateAthena* PlayerState,
        bool& OutAllowed)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsCurrentWaxGameState(GameState) ||
        !IsLiveObject(
            const_cast<AFortPlayerStateAthena*>(
                PlayerState)))
    {
        return false;
    }

    auto Mutator = ResolveWaxMutator(GameState);
    int32 EntrySize = 0;
    if (!Mutator ||
        !ValidateWaxPlayerData(Mutator, EntrySize))
    {
        return false;
    }

    auto Entry = FindWaxPlayerDataEntry(
        Mutator,
        const_cast<AFortPlayerStateAthena*>(
            PlayerState),
        EntrySize);
    if (!Entry)
        return false;

    OutAllowed =
        !Entry->bPermanentlyWaxed &&
        Entry->CurrentLives > 0;
    return true;
}

bool FFortAthenaNativeLTMCompatibility::
    TryGetDiscoRespawnAllowed(
        const AFortPlayerStateAthena* PlayerState,
        bool& OutAllowed)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsLiveObject(GameState) ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeDiscoDescriptor(
            GNativeLTMCompatibilityState.Descriptor) ||
        !IsLiveObject(
            const_cast<AFortPlayerStateAthena*>(
                PlayerState)))
    {
        return false;
    }

    auto Mutator = FindAuthoredNativeLTMMutator(
        GameState,
        GNativeLTMCompatibilityState.Descriptor);
    const UClass* DiscoClass =
        AFortAthenaMutator_Disco::StaticClass();
    auto Disco =
        IsLiveObject(Mutator) &&
                DiscoClass &&
                Mutator->IsA(DiscoClass)
            ? static_cast<AFortAthenaMutator_Disco*>(
                  Mutator)
            : nullptr;
    UFunction* Function =
        Disco
            ? Disco->GetFunction("IsRespawningAllowed")
            : nullptr;
    if (!Disco ||
        !HasNoInputBoolReturn(Function))
        return false;

    OutAllowed = Disco->IsRespawningAllowed();
    return true;
}

void FFortAthenaNativeLTMCompatibility::
    HandleWaxPlayerReady(
        AFortPlayerControllerAthena* PlayerController)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsCurrentWaxGameState(GameState) ||
        !IsWaxGameplayPhase(GameState) ||
        !IsLiveObject(PlayerController) ||
        !PlayerController->HasPlayerState() ||
        !IsLiveObject(PlayerController->PlayerState))
    {
        return;
    }

    auto PlayerState =
        static_cast<AFortPlayerStateAthena*>(
            PlayerController->PlayerState);
    auto& State = GWaxCompatibilityState;
    auto ReadyPawn =
        IsPlayableWaxPawn(PlayerController->MyFortPawn)
            ? PlayerController->MyFortPawn
            : nullptr;
    auto Processed =
        State.ProcessedEliminationPawns.find(PlayerState);
    if (ReadyPawn &&
        Processed !=
            State.ProcessedEliminationPawns.end() &&
        Processed->second.Get() != ReadyPawn)
    {
        // Clear the death claim only after a distinct, playable replacement
        // pawn is observed. Repeated acknowledgements of the dying pawn must
        // not make CommonDeadPawn dispatchable again.
        State.ProcessedEliminationPawns.erase(Processed);
    }

    auto Mutator = ResolveWaxMutator(GameState);
    int32 EntrySize = 0;
    if (!Mutator ||
        !ValidateWaxPlayerData(Mutator, EntrySize))
    {
        return;
    }

    const double Now =
        UGameplayStatics::GetTimeSeconds(World);
    if (FindWaxPlayerDataEntry(
            Mutator, PlayerState, EntrySize))
    {
        State.MissingPlayerDataSince.erase(PlayerState);
        return;
    }

    ObserveMissingWaxPlayerData(PlayerState, Now);
    if (!GNativeLTMCompatibilityState.bCompatibilityComplete)
        return;
    if (State.CompatibilityCompletedObservedTime < 0.0)
        State.CompatibilityCompletedObservedTime = Now;

    bool bFallbackReady = State.bCompatibilityOwnsPlayerData;
    if (!bFallbackReady &&
        Mutator->PlayerData.Entries.Num() == 0)
    {
        const double FallbackGraceStart =
            (std::max)(
                State.MutatorResolvedTime,
                State.CompatibilityCompletedObservedTime);
        bFallbackReady =
            FallbackGraceStart >= 0.0 &&
            Now >= FallbackGraceStart +
                WaxPlayerDataGraceSeconds;
    }
    else if (!bFallbackReady)
    {
        bFallbackReady =
            WaxPlayerDataGraceElapsed(
                PlayerState, Now);
    }

    if (bFallbackReady)
    {
        EnsureWaxPlayerData(
            Mutator,
            PlayerState,
            "pawn-ready");
    }
}

void FFortAthenaNativeLTMCompatibility::
    HandleWaxElimination(
        AFortPlayerStateAthena* VictimPlayerState,
        AFortPlayerPawnAthena* VictimPawn)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        !IsCurrentWaxGameState(GameState) ||
        !IsWaxGameplayPhase(GameState) ||
        !IsLiveObject(VictimPlayerState) ||
        !IsLiveObject(VictimPawn) ||
        VictimPawn->IsDBNO())
    {
        return;
    }

    auto Mutator = ResolveWaxMutator(GameState);
    int32 EntrySize = 0;
    if (!Mutator ||
        !ValidateWaxPlayerData(Mutator, EntrySize) ||
        !Mutator->GetFunction("CommonDeadPawn"))
    {
        return;
    }

    auto& State = GWaxCompatibilityState;
    if (!FindWaxPlayerDataEntry(
            Mutator,
            VictimPlayerState,
            EntrySize))
    {
        const double Now =
            UGameplayStatics::GetTimeSeconds(World);
        ObserveMissingWaxPlayerData(
            VictimPlayerState, Now);

        // A confirmed full death cannot wait for the normal join grace: the
        // respawn decision is sampled immediately after this callback. Once
        // native setup is complete, append/initialize the missing record now
        // so CommonDeadPawn consumes this life exactly once.
        if (GNativeLTMCompatibilityState
                .bCompatibilityComplete)
        {
            EnsureWaxPlayerData(
                Mutator,
                VictimPlayerState,
                "elimination-immediate");
            if (!ValidateWaxPlayerData(
                    Mutator,
                    EntrySize))
            {
                return;
            }
        }
    }

    // CommonDeadPawn assumes its fast-array record already exists. Missing
    // native entries are repaired append-only after the per-player grace.
    if (!FindWaxPlayerDataEntry(
            Mutator,
            VictimPlayerState,
            EntrySize))
    {
        SDK::DbgLog(
            "[Wax1040] skipped CommonDeadPawn: no PlayerData "
            "entry victimPS=%p pawn=%p nativeOwned=%d\n",
            static_cast<void*>(VictimPlayerState),
            static_cast<void*>(VictimPawn),
            State.bCompatibilityOwnsPlayerData ? 0 : 1);
        return;
    }

    if (IsWaxEliminationProcessed(
            VictimPlayerState, VictimPawn))
    {
        SDK::DbgLog(
            "[Wax1040] suppressed duplicate CommonDeadPawn "
            "victimPS=%p pawn=%p\n",
            static_cast<void*>(VictimPlayerState),
            static_cast<void*>(VictimPawn));
        return;
    }

    if (GWaxCommonDeadPawnHookInstalled)
    {
        State.bCompatibilityCommonDeadPawnDispatch = true;
        Mutator->CommonDeadPawn(VictimPawn);
        State.bCompatibilityCommonDeadPawnDispatch = false;

        // A Blueprint override can bypass the hooked native UFunction object.
        // The original still ran, so record it here to suppress any later
        // native duplicate.
        if (!IsWaxEliminationProcessed(
                VictimPlayerState, VictimPawn))
        {
            TryClaimWaxElimination(
                VictimPlayerState, VictimPawn);
        }
    }
    else
    {
        // If observation could not be installed, retain an exact-once local
        // guard around the compatibility dispatch.
        if (!TryClaimWaxElimination(
                VictimPlayerState, VictimPawn))
        {
            return;
        }
        Mutator->CommonDeadPawn(VictimPawn);
    }

    // Native CommonDeadPawn owns tokens, lives, placements, leaders and the
    // mutator-controlled win condition. Re-announce its authoritative array
    // snapshot so late-created HUD extensions receive the same death update.
    if (IsLiveObject(Mutator) &&
        ValidateWaxPlayerData(Mutator, EntrySize))
    {
        PublishWaxPlayerData(
            Mutator,
            EntrySize,
            true);
    }

    SDK::DbgLog(
        "[Wax1040] dispatched CommonDeadPawn once "
        "victimPS=%p pawn=%p entries=%d\n",
        static_cast<void*>(VictimPlayerState),
        static_cast<void*>(VictimPawn),
        IsLiveObject(Mutator)
            ? Mutator->PlayerData.Entries.Num()
            : -1);
}

void AFortAthenaMutator_Wax::CommonDeadPawnHook(
    UObject* Context,
    FFrame& Stack)
{
    auto CallOriginal = [&]()
    {
        if (CommonDeadPawnHookOG)
            CommonDeadPawnHookOG(Context, Stack);
    };
    auto ConsumeWithoutDispatch = [&]()
    {
        AFortPlayerPawnAthena* IgnoredPawn = nullptr;
        Stack.StepCompiledIn(&IgnoredPawn);
        Stack.IncrementCode();
    };

    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    const UClass* WaxClass = StaticClass();
    if (!CommonDeadPawnHookOG ||
        !FFortAthenaNativeLTMCompatibility::IsSupportedBuild() ||
        !World ||
        !IsCurrentWaxGameState(GameState) ||
        !IsWaxGameplayPhase(GameState) ||
        !Context ||
        !WaxClass ||
        !Context->IsA(WaxClass))
    {
        CallOriginal();
        return;
    }

    auto Mutator =
        static_cast<AFortAthenaMutator_Wax*>(Context);
    if (ResolveWaxMutator(GameState) != Mutator)
    {
        CallOriginal();
        return;
    }

    AFortPlayerPawnAthena* VictimPawn = nullptr;
    FFrame Probe = Stack;
    Probe.StepCompiledIn(&VictimPawn);
    auto VictimPlayerState =
        ResolveWaxPawnPlayerState(VictimPawn);
    if (!IsLiveObject(VictimPawn) ||
        !IsLiveObject(VictimPlayerState))
    {
        CallOriginal();
        return;
    }

    int32 EntrySize = 0;
    if (ValidateWaxPlayerData(Mutator, EntrySize) &&
        !FindWaxPlayerDataEntry(
            Mutator,
            VictimPlayerState,
            EntrySize))
    {
        const double Now =
            UGameplayStatics::GetTimeSeconds(World);
        ObserveMissingWaxPlayerData(
            VictimPlayerState, Now);

        if (GNativeLTMCompatibilityState
                .bCompatibilityComplete)
        {
            EnsureWaxPlayerData(
                Mutator,
                VictimPlayerState,
                "native-death-immediate");
            ValidateWaxPlayerData(Mutator, EntrySize);
        }

        if (!FindWaxPlayerDataEntry(
                Mutator,
                VictimPlayerState,
                EntrySize))
        {
            // The native routine indexes this array by player state. Avoid an
            // unsafe call until native setup or an immediate full-death repair
            // has produced the required record.
            ConsumeWithoutDispatch();
            SDK::DbgLog(
                "[Wax1040] CommonDeadPawn hook deferred unsafe "
                "missing-entry dispatch victimPS=%p pawn=%p\n",
                static_cast<void*>(VictimPlayerState),
                static_cast<void*>(VictimPawn));
            return;
        }
    }

    if (IsWaxEliminationProcessed(
            VictimPlayerState, VictimPawn))
    {
        ConsumeWithoutDispatch();
        SDK::DbgLog(
            "[Wax1040] CommonDeadPawn hook suppressed duplicate "
            "source=%s victimPS=%p pawn=%p\n",
            GWaxCompatibilityState
                    .bCompatibilityCommonDeadPawnDispatch
                ? "compatibility"
                : "native",
            static_cast<void*>(VictimPlayerState),
            static_cast<void*>(VictimPawn));
        return;
    }

    if (!TryClaimWaxElimination(
            VictimPlayerState, VictimPawn))
    {
        ConsumeWithoutDispatch();
        return;
    }

    const bool bCompatibilityDispatch =
        GWaxCompatibilityState
            .bCompatibilityCommonDeadPawnDispatch;
    CallOriginal();
    SDK::DbgLog(
        "[Wax1040] CommonDeadPawn hook accepted %s dispatch "
        "victimPS=%p pawn=%p\n",
        bCompatibilityDispatch ? "compatibility" : "native",
        static_cast<void*>(VictimPlayerState),
        static_cast<void*>(VictimPawn));
}

void AFortAthenaMutator_Heist::OnGamePhaseStepChanged(
    UObject* Context,
    FFrame& Stack)
{
    if (FFortAthenaNativeLTMCompatibility::IsSupportedBuild() &&
        Context)
    {
        UWorld* World = UWorld::GetWorld();
        const UClass* HeistClass = StaticClass();
        if (World && HeistClass &&
            Context->IsA(HeistClass) &&
            GNativeLTMCompatibilityState.World == World &&
            IsNativeGetawayDescriptor(
                GNativeLTMCompatibilityState.Descriptor))
        {
            // Inspect a copy so the real native implementation receives its
            // untouched frame exactly once.
            TScriptInterface<IInterface> SafeZoneInterface{};
            uint8 GamePhaseStep =
                static_cast<uint8>(
                    EAthenaGamePhaseStep::None);
            FFrame Probe = Stack;
            Probe.StepCompiledIn(&SafeZoneInterface);
            Probe.StepCompiledIn(&GamePhaseStep);

            if (GamePhaseStep >
                    static_cast<uint8>(
                        EAthenaGamePhaseStep::None) &&
                GamePhaseStep <
                    static_cast<uint8>(
                        EAthenaGamePhaseStep::Count) &&
                IsLiveObject(
                    const_cast<UObject*>(
                        SafeZoneInterface.ObjectPointer)))
            {
                auto Heist =
                    static_cast<AFortAthenaMutator_Heist*>(
                        Context);
                auto Existing =
                    GHeistCompatibilityState
                        .ObservedNative1040PhaseSteps.find(
                            Heist);
                const bool bNewObservation =
                    Existing ==
                        GHeistCompatibilityState
                            .ObservedNative1040PhaseSteps.end() ||
                    Existing->second != GamePhaseStep;
                GHeistCompatibilityState
                    .ObservedNative1040PhaseSteps[Heist] =
                    GamePhaseStep;
                const bool bManualReplay =
                    GHeistCompatibilityState
                        .ManualNative1040PhaseInvocations
                        .contains(Heist);
                if (GamePhaseStep ==
                        static_cast<uint8>(
                            EAthenaGamePhaseStep::Setup) &&
                    !bManualReplay)
                {
                    GHeistCompatibilityState
                        .CompletedNative1040SetupRecovery
                        .insert(Heist);
                    GHeistCompatibilityState
                        .Native1040SetupRecoveryAttempts.erase(
                            Heist);
                    GHeistCompatibilityState
                        .Native1040NextSetupRecoveryAttemptTimes
                        .erase(Heist);
                }
                if (bNewObservation)
                {
                    SDK::DbgLog(
                        "[Getaway1040] %s phase callback entered "
                        "on %s step=%u\n",
                        bManualReplay
                            ? "replayed"
                            : "native",
                        Heist->Name.ToString().c_str(),
                        static_cast<unsigned>(
                            GamePhaseStep));
                }
            }
        }
    }

    if (OnGamePhaseStepChangedOG)
        OnGamePhaseStepChangedOG(Context, Stack);
}

namespace
{
    void ObserveAuthoredNativeLTMPhaseStep(
        UObject* Context,
        const FFrame& Stack,
        const UClass* ExpectedClass,
        bool bExpectAshton)
    {
        if (!FFortAthenaNativeLTMCompatibility::IsSupportedBuild() ||
            !Context || !ExpectedClass ||
            !Context->IsA(ExpectedClass))
        {
            return;
        }

        UWorld* World = UWorld::GetWorld();
        const auto Descriptor =
            GNativeLTMCompatibilityState.Descriptor;
        if (!World ||
            GNativeLTMCompatibilityState.World != World ||
            (bExpectAshton
                 ? !IsNativeAshtonDescriptor(Descriptor)
                 : !IsNativeDiscoDescriptor(Descriptor)))
        {
            return;
        }

        TScriptInterface<IInterface> SafeZoneInterface{};
        uint8 GamePhaseStep =
            static_cast<uint8>(
                EAthenaGamePhaseStep::None);
        FFrame Probe = Stack;
        Probe.StepCompiledIn(&SafeZoneInterface);
        Probe.StepCompiledIn(&GamePhaseStep);
        if (GamePhaseStep <=
                static_cast<uint8>(
                    EAthenaGamePhaseStep::None) ||
            GamePhaseStep >=
                static_cast<uint8>(
                    EAthenaGamePhaseStep::Count) ||
            !IsLiveObject(
                const_cast<UObject*>(
                    SafeZoneInterface.ObjectPointer)))
        {
            return;
        }

        auto Mutator =
            static_cast<AFortGameplayMutator*>(Context);
        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        const auto Existing =
            PhaseState.LastObservedPhaseSteps.find(
                Mutator);
        const bool bNewObservation =
            Existing ==
                PhaseState.LastObservedPhaseSteps.end() ||
            Existing->second != GamePhaseStep;
        const bool bManualReplay =
            PhaseState.ManualPhaseStepInvocations.contains(
                Mutator);
        PhaseState.LastObservedPhaseSteps[Mutator] =
            GamePhaseStep;
        if (GamePhaseStep ==
            static_cast<uint8>(
                EAthenaGamePhaseStep::Setup))
        {
            PhaseState.ObservedSetup.insert(Mutator);
        }

        if (bNewObservation)
        {
            SDK::DbgLog(
                "[NativeLTM] %s: %s phase-step callback "
                "entered on %s step=%u\n",
                bExpectAshton
                    ? "Avengers: Endgame"
                    : "Disco Domination",
                bManualReplay ? "replayed" : "native",
                Mutator->Name.ToString().c_str(),
                static_cast<unsigned>(GamePhaseStep));
        }
    }

    void ObserveDiscoGamePhase(
        UObject* Context,
        const FFrame& Stack)
    {
        if (!FFortAthenaNativeLTMCompatibility::IsSupportedBuild() ||
            !Context)
        {
            return;
        }

        UWorld* World = UWorld::GetWorld();
        const UClass* DiscoClass =
            AFortAthenaMutator_Disco::StaticClass();
        if (!World || !DiscoClass ||
            !Context->IsA(DiscoClass) ||
            GNativeLTMCompatibilityState.World != World ||
            !IsNativeDiscoDescriptor(
                GNativeLTMCompatibilityState.Descriptor))
        {
            return;
        }

        uint8 GamePhase =
            static_cast<uint8>(EAthenaGamePhase::None);
        FFrame Probe = Stack;
        Probe.StepCompiledIn(&GamePhase);
        if (GamePhase <=
                static_cast<uint8>(
                    EAthenaGamePhase::None) ||
            GamePhase >=
                static_cast<uint8>(
                    EAthenaGamePhase::Count))
        {
            return;
        }

        auto Mutator =
            static_cast<AFortGameplayMutator*>(Context);
        auto& PhaseState =
            GAuthoredNativeLTMPhaseState;
        const auto Existing =
            PhaseState.LastObservedGamePhases.find(
                Mutator);
        const bool bNewObservation =
            Existing ==
                PhaseState.LastObservedGamePhases.end() ||
            Existing->second != GamePhase;
        const bool bManualReplay =
            PhaseState.ManualGamePhaseInvocations.contains(
                Mutator);
        PhaseState.LastObservedGamePhases[Mutator] =
            GamePhase;
        if (GamePhase ==
            static_cast<uint8>(EAthenaGamePhase::Setup))
        {
            PhaseState.ObservedGamePhaseSetup.insert(
                Mutator);
        }

        if (bNewObservation)
        {
            SDK::DbgLog(
                "[Disco1040] %s game-phase callback entered "
                "on %s phase=%u\n",
                bManualReplay ? "replayed" : "native",
                Mutator->Name.ToString().c_str(),
                static_cast<unsigned>(GamePhase));
        }
    }
}

void AFortAthenaMutator_Ashton::OnGamePhaseStepChanged(
    UObject* Context,
    FFrame& Stack)
{
    ObserveAuthoredNativeLTMPhaseStep(
        Context,
        Stack,
        StaticClass(),
        true);
    if (OnGamePhaseStepChangedOG)
        OnGamePhaseStepChangedOG(Context, Stack);
}

void AFortAthenaMutator_Ashton::OnPickupDestroying(
    UObject* Context,
    FFrame& Stack)
{
    AFortPickupAthena* Pickup = nullptr;
    UFortItemDefinition* ItemDefinition = nullptr;
    FFrame Probe = Stack;
    Probe.StepCompiledIn(&Pickup);
    Probe.StepCompiledIn(&ItemDefinition);

    UWorld* World = nullptr;
    AFortGameStateAthena* GameState = nullptr;
    AFortAthenaMutator_Ashton* Ashton = nullptr;
    int32 StoneType = -1;
    const bool bCurrentStone =
        ResolveCurrentAshtonStoneRuntime(
            ItemDefinition,
            World,
            GameState,
            Ashton,
            StoneType) &&
        Context == Ashton;
    auto StoneBefore =
        bCurrentStone
            ? FindAshtonStoneState(
                  Ashton, StoneType)
            : nullptr;
    const bool bWasAlreadyCaptured =
        bCurrentStone &&
        GAuthoredNativeLTMPhaseState
            .bAshtonStoneCaptureLocked[
                StoneType];

    AFortPlayerPawnAthena* CollectorPawn =
        IsLiveObject(Pickup)
            ? Pickup->PickupLocationData
                  .PickupTarget
            : nullptr;
    if (!IsLiveObject(CollectorPawn) &&
        IsLiveObject(Pickup) &&
        Pickup->PickupLocationData
            .HasItemOwner())
    {
        CollectorPawn =
            Pickup->PickupLocationData
                .ItemOwner;
    }
    AFortPlayerControllerAthena* Collector = nullptr;
    const bool bValidVillainCollector =
        bCurrentStone &&
        ResolveAshtonVillainCollector(
            CollectorPawn, Collector);
    auto LeaderBefore =
        bCurrentStone &&
                Ashton->HasVillainLeaderPC() &&
                !GAuthoredNativeLTMPhaseState
                     .bAshtonLeaderVacant &&
                IsValidAshtonLeaderController(
                    Ashton->VillainLeaderPC)
            ? Ashton->VillainLeaderPC
            : nullptr;

    if (bCurrentStone &&
        !bValidVillainCollector)
    {
        // This is the last server-side boundary before the native mutator
        // changes StoneList. Keep a bypassed hero/null-target destroy from
        // claiming the objective; the spawned row remains eligible for the
        // authoritative pickup watchdog to restore.
        AFortPickupAthena* ConsumedPickup = nullptr;
        UFortItemDefinition* ConsumedDefinition =
            nullptr;
        Stack.StepCompiledIn(&ConsumedPickup);
        Stack.StepCompiledIn(
            &ConsumedDefinition);
        Stack.IncrementCode();
        SDK::DbgLog(
            "[Ashton1040] blocked native stone destroy "
            "without villain collector type=%d pickup=%p "
            "pawn=%p\n",
            StoneType,
            static_cast<void*>(Pickup),
            static_cast<void*>(CollectorPawn));
        return;
    }

    if (bCurrentStone &&
        bValidVillainCollector &&
        bWasAlreadyCaptured)
    {
        AFortPickupAthena* ConsumedPickup = nullptr;
        UFortItemDefinition* ConsumedDefinition =
            nullptr;
        Stack.StepCompiledIn(&ConsumedPickup);
        Stack.StepCompiledIn(
            &ConsumedDefinition);
        Stack.IncrementCode();
        const int32 RemovedStoneRows =
            RemoveAshtonInventoryRows(
                Collector, nullptr, true);
        RetireDuplicateAshtonPickup(
            Pickup);
        SDK::DbgLog(
            "[Ashton1040] blocked duplicate native "
            "stone destroy type=%d collector=%p "
            "pickup=%p removedRows=%d\n",
            StoneType,
            static_cast<void*>(Collector),
            static_cast<void*>(Pickup),
            RemovedStoneRows);
        return;
    }

    if (OnPickupDestroyingOG)
        OnPickupDestroyingOG(Context, Stack);

    if (!bCurrentStone ||
        !bValidVillainCollector)
    {
        return;
    }

    auto Stone =
        FindAshtonStoneState(
            Ashton, StoneType);
    bool bUsedStateFallback = false;
    if (!Stone ||
        Stone->StoneState !=
            AshtonStoneStateCaptured)
    {
        // Entering this delegate with an authoritative exact pickup and a
        // valid villain PickupTarget is itself completion proof. Repair a
        // stripped native body here; do not redispatch the multicast.
        bUsedStateFallback =
            MarkAshtonStoneCapturedFallback(
                Ashton, StoneType);
        Stone =
            FindAshtonStoneState(
                Ashton, StoneType);
    }
    if (!Stone ||
        Stone->StoneState !=
            AshtonStoneStateCaptured)
    {
        return;
    }

    GAuthoredNativeLTMPhaseState
        .bAshtonStoneCaptureLocked[
            StoneType] = true;
    RemoveAshtonInventoryRows(
        Collector, nullptr, true);
    const bool bCentralDispatchOwnsFinalize =
        GAuthoredNativeLTMPhaseState
            .bAshtonStoneDestroyDispatchInFlight[
                StoneType];
    if (!bCentralDispatchOwnsFinalize)
    {
        FinalizeAshtonStoneLeader(
            Ashton,
            Collector,
            LeaderBefore,
            "native-pickup-destroy");
    }
    SDK::DbgLog(
        "[Ashton1040] observed native stone destroy "
        "type=%d collector=%p pickup=%p "
        "leaderBefore=%p leaderAfter=%p "
        "centralFinalize=%d stateFallback=%d\n",
        StoneType,
        static_cast<void*>(Collector),
        static_cast<void*>(Pickup),
        static_cast<void*>(LeaderBefore),
        static_cast<void*>(
            Ashton->HasVillainLeaderPC()
                ? Ashton->VillainLeaderPC
                : nullptr),
        bCentralDispatchOwnsFinalize ? 1 : 0,
        bUsedStateFallback ? 1 : 0);
}

void AFortAthenaMutator_Ashton::
    SelectNextVillainLeader(
        UObject* Context,
        FFrame& Stack)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState
                  ->Cast<AFortGameStateAthena>()
            : nullptr;
    const UClass* AshtonClass = StaticClass();
    auto Ashton =
        IsLiveObject(Context) &&
                AshtonClass &&
                Context->IsA(AshtonClass)
            ? static_cast<AFortAthenaMutator_Ashton*>(
                  Context)
            : nullptr;
    const bool bTargetMode =
        FFortAthenaNativeLTMCompatibility::
            IsSupportedBuild() &&
        IsLiveObject(GameState) &&
        IsLiveObject(Ashton) &&
        GNativeLTMCompatibilityState.World == World &&
        IsNativeAshtonDescriptor(
            GNativeLTMCompatibilityState.Descriptor) &&
        EnsureAshtonVillainConfiguration(GameState);

    if (!bTargetMode)
    {
        if (SelectNextVillainLeaderOG)
            SelectNextVillainLeaderOG(Context, Stack);
        else
            Stack.IncrementCode();
        return;
    }

    auto& State = GAuthoredNativeLTMPhaseState;
    auto PublishedLeader =
        Ashton->HasVillainLeaderPC()
            ? Ashton->VillainLeaderPC
            : nullptr;
    const int32 CapturedStones =
        CountCapturedAshtonStones(Ashton);
    auto StagedDeathVictim =
        State.AshtonStagedDeathVictim;
    const bool bStagedLeaderDeath =
        State.bAshtonLeaderVacant &&
        IsLiveObject(StagedDeathVictim) &&
        State.AshtonEliminatedLeaders.contains(
            StagedDeathVictim);
    if (bStagedLeaderDeath)
    {
        if (CapturedStones < 6)
        {
            // The pre-native death phase deliberately retains the base gadget
            // and pointer until all authored fallen cues/messages have run.
            // Before the final stone, only its next collector may succeed it.
            Stack.IncrementCode();
            SDK::DbgLog(
                "[Ashton1040] suppressed leader selection "
                "during staged Thanos death victim=%p "
                "captured=%d\n",
                static_cast<void*>(StagedDeathVictim),
                CapturedStones);
            return;
        }

        // With no stone left to choose a collector, permit the authored
        // fallback but authenticate its exact result. Keep the external
        // vacancy set until post-native death cleanup removes the old gadget;
        // only then is the candidate granted the replacement.
        if (SelectNextVillainLeaderOG)
            SelectNextVillainLeaderOG(Context, Stack);
        else
            Stack.IncrementCode();
        auto Candidate =
            Ashton->HasVillainLeaderPC()
                ? Ashton->VillainLeaderPC
                : nullptr;
        State.AshtonAuthorizedAllStoneFallback =
            Candidate &&
                    Candidate != StagedDeathVictim &&
                    IsValidAshtonLeaderController(
                        Candidate)
                ? Candidate
                : nullptr;
        SDK::DbgLog(
            "[Ashton1040] staged authored all-stones "
            "leader candidate victim=%p candidate=%p "
            "authorized=%d\n",
            static_cast<void*>(StagedDeathVictim),
            static_cast<void*>(Candidate),
            State.AshtonAuthorizedAllStoneFallback
                    ? 1
                    : 0);
        return;
    }
    const bool bHasLivingLeader =
        !State.bAshtonLeaderVacant &&
        IsValidAshtonLeaderController(
            PublishedLeader);
    if (!bHasLivingLeader)
    {
        State.bAshtonLeaderVacant = true;
        if (PublishedLeader)
        {
            Ashton->VillainLeaderPC = nullptr;
            Ashton->ForceNetUpdate();
            RemoveAllAshtonLeaderArtifacts(
                Ashton, PublishedLeader);
        }
    }

    if (bHasLivingLeader ||
        (State.bAshtonLeaderVacant &&
         CapturedStones < 6))
    {
        // Native delayed selection can otherwise create a random/second
        // Thanos. Before the final stone, a vacant slot is filled only by the
        // next villain who completes a stone capture.
        Stack.IncrementCode();
        SDK::DbgLog(
            "[Ashton1040] suppressed native leader selection "
            "livingLeader=%d vacant=%d captured=%d\n",
            bHasLivingLeader ? 1 : 0,
            State.bAshtonLeaderVacant ? 1 : 0,
            CapturedStones);
        return;
    }

    if (SelectNextVillainLeaderOG)
        SelectNextVillainLeaderOG(Context, Stack);
    else
        Stack.IncrementCode();

    // Once every stone is gone there can be no next collector. Permit the
    // authored delayed fallback, then immediately normalize it to one leader.
    auto FallbackLeader =
        Ashton->HasVillainLeaderPC()
            ? Ashton->VillainLeaderPC
            : nullptr;
    if (CapturedStones >= 6 &&
        IsValidAshtonLeaderController(
            FallbackLeader))
    {
        State.AshtonAuthorizedAllStoneFallback =
            nullptr;
        State.bAshtonLeaderVacant = false;
        ReconcileAshtonVillainPlayers(
            GameState,
            "all-stones-native-leader-fallback");
    }
}

void AFortAthenaMutator_Disco::OnGamePhaseChanged(
    UObject* Context,
    FFrame& Stack)
{
    ObserveDiscoGamePhase(Context, Stack);
    if (OnGamePhaseChangedOG)
        OnGamePhaseChangedOG(Context, Stack);
}

void AFortAthenaMutator_Disco::OnGamePhaseStepChanged(
    UObject* Context,
    FFrame& Stack)
{
    ObserveAuthoredNativeLTMPhaseStep(
        Context,
        Stack,
        StaticClass(),
        false);
    if (OnGamePhaseStepChangedOG)
        OnGamePhaseStepChangedOG(Context, Stack);
}

void AFortAthenaMutator_Barrier::OnGamePhaseStepChanged(
    UObject* Context,
    FFrame& Stack)
{
    auto CallOriginal = [&]()
    {
        if (OnGamePhaseStepChangedOG)
            OnGamePhaseStepChangedOG(Context, Stack);
    };

    if (!FFortAthenaNativeLTMCompatibility::IsSupportedBuild())
    {
        CallOriginal();
        return;
    }

    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    const UClass* BarrierClass = StaticClass();
    if (!World || !GameState || !BarrierClass || !Context ||
        !Context->IsA(BarrierClass) ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeFoodFightDescriptor(
            GNativeLTMCompatibilityState.Descriptor) ||
        !GameState->HasGamePhaseStep() ||
        GameState->GamePhaseStep != static_cast<uint8>(
            EAthenaGamePhaseStep::BusLocked))
    {
        CallOriginal();
        return;
    }

    // Read the safe-zone argument from a copy.  The native handler receives
    // the original, untouched frame exactly once after the arena is ready.
    TScriptInterface<IInterface> SafeZoneInterface{};
    FFrame Probe = Stack;
    Probe.StepCompiledIn(&SafeZoneInterface);

    auto Barrier =
        static_cast<AFortAthenaMutator_Barrier*>(Context);
    EnsureDeepFriedArena(
        World, GameState, Barrier, SafeZoneInterface);

    CallOriginal();

    // The native phase handler can finish publishing SafeZoneLocations.
    // A second idempotent pass lets a pre-handler deferral resolve without
    // ever falling back to an out-of-zone objective pair.
    EnsureDeepFriedArena(
        World, GameState, Barrier, SafeZoneInterface);

    SDK::DbgLog(
        "[FoodFight] native BusLocked result: context=%p wall=%p "
        "team0Flag=%p team0Objective=%p "
        "team1Flag=%p team1Objective=%p bound=%d\n",
        static_cast<void*>(Barrier),
        static_cast<void*>(GDeepFriedArenaState.Wall),
        static_cast<void*>(GDeepFriedArenaState.Flags[0]),
        static_cast<void*>(GDeepFriedArenaState.Objectives[0]),
        static_cast<void*>(GDeepFriedArenaState.Flags[1]),
        static_cast<void*>(GDeepFriedArenaState.Objectives[1]),
        GDeepFriedArenaState.bBindingsComplete ? 1 : 0);
}

namespace
{
    struct FMutatorGrantAmmo
    {
        int32 LoadedAmmo = 0;
        int32 PhantomReserveAmmo = 0;
    };

    FMutatorGrantAmmo ResolveMutatorGrantAmmo(
        const UFortItemDefinition* ItemDefinition)
    {
        FMutatorGrantAmmo Result{};
        if (!ItemDefinition)
            return Result;

        auto WeaponDefinition =
            ItemDefinition->Cast<UFortWeaponItemDefinition>();
        if (!WeaponDefinition)
        {
            if (auto Gadget =
                    ItemDefinition->Cast<UFortGadgetItemDefinition>())
            {
                WeaponDefinition =
                    Gadget->GetWeaponItemDefinition();
            }
        }
        if (!WeaponDefinition)
            return Result;

        auto Stats = AFortInventory::GetStats(WeaponDefinition);
        if (!Stats)
            return Result;

        Result.LoadedAmmo = Stats->ClipSize;
        if (WeaponDefinition->HasbUsesPhantomReserveAmmo() &&
            WeaponDefinition->bUsesPhantomReserveAmmo)
        {
            Result.PhantomReserveAmmo =
                (std::max)(
                    (Stats->InitialClips - 1) *
                        Result.LoadedAmmo,
                    0);
        }
        return Result;
    }

    bool IsDeepFriedSingletonGrant(
        const UFortItemDefinition* ItemDefinition)
    {
        if (!ItemDefinition ||
            GNativeLTMCompatibilityState.World !=
                UWorld::GetWorld() ||
            !IsNativeDeepFriedDescriptor(
                GNativeLTMCompatibilityState.Descriptor))
        {
            return false;
        }

        const auto Name = ItemDefinition->Name.ToWString();
        return Name == L"WID_Hook_Gun_Slide";
    }

    bool IsCurrentNormalFoodFightGameState(
        AFortGameStateAthena* GameState);

    bool IsSupportedBigTeamGliderGrant(
        const UFortItemDefinition* ItemDefinition)
    {
        UWorld* World = UWorld::GetWorld();
        auto GameState =
            World && World->GameState
                ? World->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        if (!ItemDefinition ||
            ItemDefinition->Name.ToWString() !=
                L"Athena_Glider_Item_BigTeamMode")
        {
            return false;
        }

        if (IsCurrentNormalFoodFightGameState(GameState))
            return true;

        // Disco's authored BigTeamGliderModifier grants this exact gadget.
        // Route it through the same initialized 50-charge entry used by
        // normal Food Fight instead of the generic zero-usage grant.
        return World &&
            GNativeLTMCompatibilityState.World == World &&
            IsNativeDiscoDescriptor(
                GNativeLTMCompatibilityState.Descriptor);
    }

    bool HasInitializedBigTeamGliderCharges(
        const FFortItemEntry& Entry)
    {
        if (!Entry.HasGenericAttributeValues() ||
            !Entry.HasStateValues() ||
            !FFortItemEntryStateValue::StaticStruct() ||
            !FFortItemEntryStateValue::HasIntValue() ||
            !FFortItemEntryStateValue::HasStateType())
        {
            return false;
        }

        const auto& Attributes =
            Entry.GenericAttributeValues;
        if (!IsSaneArray(
                Attributes.Num(), Attributes.Max(), 16) ||
            Attributes.Num() < 1 ||
            !IsReadableArrayStorage(
                Attributes.Data,
                Attributes.Num(),
                sizeof(float)) ||
            !std::isfinite(Attributes[0]) ||
            Attributes[0] <= 0.0f)
        {
            return false;
        }

        const int32 StateValueSize =
            FFortItemEntryStateValue::Size();
        const auto& StateValues = Entry.StateValues;
        if (StateValueSize <= 0 ||
            StateValueSize > 0x100 ||
            !IsSaneArray(
                StateValues.Num(), StateValues.Max(), 64) ||
            !IsReadableArrayStorage(
                StateValues.Data,
                StateValues.Num(),
                static_cast<size_t>(StateValueSize)))
        {
            return false;
        }

        for (int32 Index = 0;
             Index < StateValues.Num();
             ++Index)
        {
            const auto& State =
                StateValues.Get(Index, StateValueSize);
            if (State.StateType == 12 &&
                State.IntValue == 1)
            {
                return true;
            }
        }
        return false;
    }

    void GiveConfiguredMutatorItem(
        AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition,
        int32 Count)
    {
        if (!PlayerController || !PlayerController->WorldInventory ||
            !ItemDefinition || Count <= 0)
        {
            return;
        }

        auto Inventory = PlayerController->WorldInventory;
        auto Ammo =
            ResolveMutatorGrantAmmo(ItemDefinition);
        const bool bDeepFriedGrappler =
            IsDeepFriedSingletonGrant(ItemDefinition);
        const bool bBigTeamGlider =
            IsSupportedBigTeamGliderGrant(ItemDefinition);
        UWorld* World = UWorld::GetWorld();
        const bool bDiscoBigTeamGlider =
            bBigTeamGlider &&
            World &&
            GNativeLTMCompatibilityState.World ==
                World &&
            IsNativeDiscoDescriptor(
                GNativeLTMCompatibilityState
                    .Descriptor);
        if (bDeepFriedGrappler && Ammo.LoadedAmmo <= 0)
            Ammo.LoadedAmmo = 1;

        // Deep Fried owns exactly one slide grappler per player.  Repair an
        // already-created entry instead of allowing a repeated phase callback
        // or modifier registration to create a second quick-bar item.
        if (bDeepFriedGrappler)
        {
            auto Existing =
                Inventory->Inventory.ReplicatedEntries.Search(
                    [&](FFortItemEntry& Entry)
                    {
                        return Entry.ItemDefinition ==
                            ItemDefinition;
                    },
                    FFortItemEntry::Size());
            if (Existing)
            {
                FGuid ExistingGuid = Existing->ItemGuid;
                bool bChanged = false;
                if (Existing->LoadedAmmo < Ammo.LoadedAmmo)
                {
                    Existing->LoadedAmmo = Ammo.LoadedAmmo;
                    bChanged = true;
                }
                if (Existing->HasPhantomReserveAmmo() &&
                    Existing->PhantomReserveAmmo <
                        Ammo.PhantomReserveAmmo)
                {
                    Existing->PhantomReserveAmmo =
                        Ammo.PhantomReserveAmmo;
                    bChanged = true;
                }

                auto ItemInstance =
                    Inventory->Inventory.ItemInstances.Search(
                        [&](UFortWorldItem* Item)
                        {
                            return Item &&
                                Item->ItemEntry.ItemGuid ==
                                    ExistingGuid;
                        });
                if (ItemInstance && *ItemInstance)
                {
                    (*ItemInstance)->ItemEntry.LoadedAmmo =
                        Existing->LoadedAmmo;
                    if ((*ItemInstance)->ItemEntry
                            .HasPhantomReserveAmmo())
                    {
                        (*ItemInstance)->ItemEntry
                            .PhantomReserveAmmo =
                            Existing->HasPhantomReserveAmmo()
                                ? Existing->PhantomReserveAmmo
                                : 0;
                    }
                    (*ItemInstance)->ItemEntry.bIsDirty = true;
                }

                const int32 LoggedLoadedAmmo =
                    Existing->LoadedAmmo;
                const int32 LoggedPhantomReserve =
                    Existing->HasPhantomReserveAmmo()
                        ? Existing->PhantomReserveAmmo
                        : 0;
                SDK::DbgLog(
                    "[FoodFight] kept one grappler for PC=%p "
                    "loadedAmmo=%d phantomReserve=%d repaired=%d\n",
                    static_cast<void*>(PlayerController),
                    LoggedLoadedAmmo,
                    LoggedPhantomReserve,
                    bChanged ? 1 : 0);
                if (bChanged)
                    Inventory->Update(Existing);
                return;
            }
        }

        if (bBigTeamGlider)
        {
            auto Existing =
                Inventory->Inventory.ReplicatedEntries.Search(
                    [&](FFortItemEntry& Entry)
                    {
                        return Entry.ItemDefinition ==
                            ItemDefinition;
                    },
                    FFortItemEntry::Size());
            if (Existing &&
                HasInitializedBigTeamGliderCharges(*Existing))
            {
                if (bDiscoBigTeamGlider)
                {
                    GAuthoredNativeLTMPhaseState
                        .DiscoGliderGrantedPlayers
                        .insert(PlayerController);
                }
                SDK::DbgLog(
                    "[NativeLTM] kept initialized big-team glider "
                    "PC=%p charges=%.1f\n",
                    static_cast<void*>(PlayerController),
                    Existing->GenericAttributeValues[0]);
                return;
            }

            if (!FFortItemEntry::HasGenericAttributeValues() ||
                !FFortItemEntry::HasStateValues() ||
                !FFortItemEntryStateValue::StaticStruct() ||
                !FFortItemEntryStateValue::HasIntValue() ||
                !FFortItemEntryStateValue::HasStateType())
            {
                SDK::DbgLog(
                    "[NativeLTM] big-team glider charge layout "
                    "unavailable "
                    "PC=%p stateValueSize=0\n",
                    static_cast<void*>(PlayerController));
                return;
            }

            const int32 StateValueSize =
                FFortItemEntryStateValue::Size();
            if (StateValueSize <= 0 ||
                StateValueSize > 0x100)
            {
                SDK::DbgLog(
                    "[NativeLTM] big-team glider charge layout "
                    "unavailable "
                    "PC=%p stateValueSize=%d\n",
                    static_cast<void*>(PlayerController),
                    StateValueSize);
                return;
            }

            // A prior partial phase callback can leave the gadget registered
            // with zero CurrentCharges. Remove that entry so the authored
            // entry and native gadget lifecycle are applied exactly once.
            if (Existing)
            {
                const FGuid ExistingGuid =
                    Existing->ItemGuid;
                Inventory->Remove(ExistingGuid);
            }

            // Athena_Glider_Item_BigTeamMode is authored as one gadget with
            // 50 CurrentCharges. State type 12 marks the generic attribute
            // value as initialized for this entry.
            TArray<FFortItemEntryStateValue> StateValues{};
            auto StateValue =
                static_cast<FFortItemEntryStateValue*>(
                    FMemory::Malloc(StateValueSize));
            if (!StateValue)
            {
                SDK::DbgLog(
                    "[NativeLTM] failed to allocate big-team "
                    "glider state "
                    "PC=%p stateValueSize=%d\n",
                    static_cast<void*>(PlayerController),
                    StateValueSize);
                return;
            }

            memset(StateValue, 0, StateValueSize);
            StateValue->StateType = 12;
            StateValue->IntValue = 1;
            StateValues.Add(*StateValue, StateValueSize);
            FMemory::Free(StateValue);

            TArray<float> GenericAttributeValues{};
            GenericAttributeValues.Add(50.0f);

            // Defer replication and skip the generic item-added callback.
            // ApplyGadgetData is the 10.40 path that binds the glider's
            // authored ability/attribute sets to this inventory owner.
            auto GrantedItem = Inventory->GiveItem(
                ItemDefinition,
                1,
                0,
                0,
                true,
                false,
                0,
                StateValues,
                false,
                GenericAttributeValues);
            const bool bGadgetApplied =
                GrantedItem &&
                Inventory->InitializeGadgetItem(
                    GrantedItem, true);

            StateValues.Free();
            GenericAttributeValues.Free();

            float GrantedCharges = 0.0f;
            bool bHasInitializedMetadata = false;
            if (GrantedItem &&
                GrantedItem->ItemEntry
                    .HasGenericAttributeValues())
            {
                const auto& Attributes =
                    GrantedItem->ItemEntry
                        .GenericAttributeValues;
                if (IsSaneArray(
                        Attributes.Num(),
                        Attributes.Max(),
                        16) &&
                    Attributes.Num() > 0 &&
                    IsReadableArrayStorage(
                        Attributes.Data,
                        Attributes.Num(),
                        sizeof(float)) &&
                    std::isfinite(Attributes[0]))
                {
                    GrantedCharges = Attributes[0];
                }
            }
            if (GrantedItem)
            {
                bHasInitializedMetadata =
                    HasInitializedBigTeamGliderCharges(
                        GrantedItem->ItemEntry);
            }

            const bool bGranted = GrantedItem != nullptr;
            bool bCleanedFailedGrant = false;
            if (GrantedItem && !bGadgetApplied)
            {
                const FGuid FailedGuid =
                    GrantedItem->ItemEntry.ItemGuid;
                Inventory->Remove(FailedGuid);
                bCleanedFailedGrant = true;
            }
            if (bDiscoBigTeamGlider &&
                bGadgetApplied &&
                bHasInitializedMetadata)
            {
                GAuthoredNativeLTMPhaseState
                    .DiscoGliderGrantedPlayers
                    .insert(PlayerController);
            }

            SDK::DbgLog(
                "[NativeLTM] granted big-team glider PC=%p "
                "charges=%.1f metadata=%d gadgetApplied=%d "
                "granted=%d cleaned=%d\n",
                static_cast<void*>(PlayerController),
                GrantedCharges,
                bHasInitializedMetadata ? 1 : 0,
                bGadgetApplied ? 1 : 0,
                bGranted ? 1 : 0,
                bCleanedFailedGrant ? 1 : 0);
            return;
        }

        Inventory->GiveItem(
            ItemDefinition,
            Count,
            Ammo.LoadedAmmo,
            0,
            true,
            true,
            Ammo.PhantomReserveAmmo);

        if (bDeepFriedGrappler)
        {
            SDK::DbgLog(
                "[FoodFight] granted grappler PC=%p "
                "loadedAmmo=%d phantomReserve=%d\n",
                static_cast<void*>(PlayerController),
                Ammo.LoadedAmmo,
                Ammo.PhantomReserveAmmo);
        }
    }

    bool IsCurrentNormalFoodFightGameState(
        AFortGameStateAthena* GameState)
    {
        UWorld* World = UWorld::GetWorld();
        if (!World || !IsLiveObject(GameState) ||
            std::fabs(
                VersionInfo.FortniteVersion -
                    NativeLTMVersion) >
                NativeLTMVersionTolerance)
        {
            return false;
        }

        auto IsNormalFoodFightPlaylist =
            [](const UFortPlaylistAthena* Playlist)
            {
                const auto Descriptor =
                    FindExactNativeLTMDescriptor(Playlist);
                return IsNativeFoodFightDescriptor(Descriptor) &&
                    !IsNativeDeepFriedDescriptor(Descriptor);
            };

        bool bHasAuthoritativePlaylist = false;
        auto CheckAuthoritativePlaylist =
            [&](const UFortPlaylistAthena* Playlist)
            {
                if (!IsLiveObject(Playlist))
                    return false;

                bHasAuthoritativePlaylist = true;
                return IsNormalFoodFightPlaylist(Playlist);
            };

        if (GameState->HasCurrentPlaylistInfo())
        {
            if (FPlaylistPropertyArray::HasOverridePlaylist() &&
                CheckAuthoritativePlaylist(
                    GameState->CurrentPlaylistInfo
                        .OverridePlaylist))
            {
                return true;
            }
            if (FPlaylistPropertyArray::HasBasePlaylist() &&
                CheckAuthoritativePlaylist(
                    GameState->CurrentPlaylistInfo.BasePlaylist))
            {
                return true;
            }
        }
        if (GameState->HasCurrentPlaylistData() &&
            CheckAuthoritativePlaylist(
                GameState->CurrentPlaylistData))
        {
            return true;
        }

        if (bHasAuthoritativePlaylist)
            return false;

        const auto Descriptor =
            GNativeLTMCompatibilityState.Descriptor;
        return GNativeLTMCompatibilityState.World == World &&
            IsNativeFoodFightDescriptor(Descriptor) &&
            !IsNativeDeepFriedDescriptor(Descriptor);
    }

    bool ShouldUseNativeWaxConfiguredItemGrant()
    {
        UWorld* World = UWorld::GetWorld();
        auto GameState =
            World && World->GameState
                ? World->GameState
                      ->Cast<AFortGameStateAthena>()
                : nullptr;
        return IsCurrentWaxGameState(GameState);
    }

    bool TryEvaluateMutatorGrantCount(
        FScalableFloat& ScalableCount,
        int32& OutCount)
    {
        OutCount = 0;
        if (!std::isfinite(ScalableCount.Value))
            return false;

        const UCurveTable* CurveTable =
            ScalableCount.Curve.CurveTable;
        const UClass* CurveTableClass =
            UCurveTable::StaticClass();
        if (CurveTable &&
            (!ScalableCount.Curve.RowName.IsValid() ||
             !IsLiveObject(
                 const_cast<UCurveTable*>(CurveTable)) ||
             !CurveTableClass ||
             !CurveTable->IsA(CurveTableClass)))
        {
            return false;
        }

        const float Evaluated =
            ScalableCount.Evaluate();
        if (!std::isfinite(Evaluated) ||
            Evaluated <= 0.0f ||
            Evaluated >
                static_cast<float>(
                    (std::numeric_limits<int32>::max)()))
        {
            return false;
        }

        OutCount = static_cast<int32>(Evaluated);
        return OutCount > 0;
    }

    template <typename EntryType>
    bool ResolveMutatorGrantLayout(
        AFortAthenaMutator* Mutator,
        TArray<EntryType>& Items,
        int32& OutEntrySize)
    {
        OutEntrySize = 0;
        const UStruct* EntryStruct =
            EntryType::StaticStruct();
        if (!IsLiveObject(Mutator) ||
            !EntryStruct ||
            !EntryType::HasItemToDrop() ||
            !EntryType::HasNumberToGive() ||
            !Mutator->HasCachedGameMode() ||
            !IsLiveObject(Mutator->CachedGameMode))
        {
            return false;
        }

        const int32 EntrySize =
            EntryStruct->GetPropertiesSize();
        constexpr int32 MinimumScalableFloatBytes =
            0x18;
        if (EntrySize <= 0 ||
            EntrySize > 0x200 ||
            EntryType::ItemToDrop__Offset < 0 ||
            EntryType::NumberToGive__Offset < 0 ||
            EntryType::ItemToDrop__Offset +
                    static_cast<int32>(
                        sizeof(UFortWorldItemDefinition*)) >
                EntrySize ||
            EntryType::NumberToGive__Offset +
                    MinimumScalableFloatBytes >
                EntrySize ||
            !IsSaneArray(
                Items.Num(), Items.Max(), 128) ||
            !IsReadableArrayStorage(
                Items.GetData(),
                Items.Num(),
                static_cast<size_t>(EntrySize)))
        {
            return false;
        }

        auto& Players =
            Mutator->CachedGameMode->AlivePlayers;
        if (!IsSaneArray(
                Players.Num(), Players.Max(), 256) ||
            !IsReadableArrayStorage(
                Players.Data,
                Players.Num(),
                sizeof(AActor*)))
        {
            return false;
        }

        OutEntrySize = EntrySize;
        return true;
    }

    template <typename EntryType>
    void GrantReflectedMutatorItems(
        AFortAthenaMutator* Mutator,
        TArray<EntryType>& Items,
        int32 EntrySize)
    {
        if (!Mutator || EntrySize <= 0)
            return;

        bool bLoggedInvalidEntry = false;
        for (auto UncastedPlayer :
             Mutator->CachedGameMode->AlivePlayers)
        {
            auto PlayerController =
                IsLiveObject(UncastedPlayer)
                    ? UncastedPlayer
                          ->Cast<AFortPlayerControllerAthena>()
                    : nullptr;
            if (!IsLiveObject(PlayerController) ||
                !IsLiveObject(
                    PlayerController->WorldInventory))
            {
                continue;
            }

            for (int32 ItemIndex = 0;
                 ItemIndex < Items.Num();
                 ++ItemIndex)
            {
                auto& Item =
                    Items.Get(ItemIndex, EntrySize);
                auto ItemDefinition = Item.ItemToDrop;
                const UClass* WorldItemDefinitionClass =
                    UFortWorldItemDefinition::StaticClass();
                int32 Count = 0;
                if (!IsLiveObject(ItemDefinition) ||
                    !WorldItemDefinitionClass ||
                    !ItemDefinition->IsA(
                        WorldItemDefinitionClass) ||
                    !TryEvaluateMutatorGrantCount(
                        Item.NumberToGive, Count))
                {
                    if (!bLoggedInvalidEntry)
                    {
                        bLoggedInvalidEntry = true;
                        SDK::DbgLog(
                            "[GiveItemsMutator] skipped invalid "
                            "reflected entry index=%d stride=0x%X\n",
                            ItemIndex,
                            EntrySize);
                    }
                    continue;
                }

                GiveConfiguredMutatorItem(
                    PlayerController,
                    ItemDefinition,
                    Count);
            }
        }
    }

    bool ResolveDiscoBigTeamGliderGrant(
        AFortGameStateAthena* GameState,
        AFortAthenaMutator_GiveItemsAtGamePhaseStep*&
            OutMutator,
        UFortWorldItemDefinition*& OutItemDefinition,
        int32& OutCount)
    {
        OutMutator = nullptr;
        OutItemDefinition = nullptr;
        OutCount = 0;
        if (!IsLiveObject(GameState) ||
            !IsNativeDiscoDescriptor(
                GNativeLTMCompatibilityState.Descriptor) ||
            !GameState->HasGamePhaseStep() ||
            !GameState->HasGameplayMutators())
        {
            return false;
        }

        const UClass* GiveItemsClass =
            AFortAthenaMutator_GiveItemsAtGamePhaseStep::
                StaticClass();
        auto& Mutators = GameState->GameplayMutators;
        if (!GiveItemsClass ||
            !IsSaneArray(
                Mutators.Num(), Mutators.Max(), 128) ||
            !IsReadableArrayStorage(
                Mutators.Data,
                Mutators.Num(),
                sizeof(AFortGameplayMutator*)))
        {
            return false;
        }

        for (auto Candidate : Mutators)
        {
            auto Mutator =
                IsLiveObject(Candidate) &&
                        Candidate->IsA(GiveItemsClass)
                    ? static_cast<
                          AFortAthenaMutator_GiveItemsAtGamePhaseStep*>(
                          Candidate)
                    : nullptr;
            int32 EntrySize = 0;
            if (!Mutator ||
                !Mutator->HasPhaseToGiveItems() ||
                !Mutator->HasItemsToGive() ||
                Mutator->PhaseToGiveItems <=
                    static_cast<uint8>(
                        EAthenaGamePhaseStep::None) ||
                Mutator->PhaseToGiveItems >=
                    static_cast<uint8>(
                        EAthenaGamePhaseStep::Count) ||
                GameState->GamePhaseStep <
                    Mutator->PhaseToGiveItems ||
                !ResolveMutatorGrantLayout(
                    Mutator,
                    Mutator->ItemsToGive,
                    EntrySize))
            {
                continue;
            }

            for (int32 ItemIndex = 0;
                 ItemIndex < Mutator->ItemsToGive.Num();
                 ++ItemIndex)
            {
                auto& Item =
                    Mutator->ItemsToGive.Get(
                        ItemIndex, EntrySize);
                if (!IsLiveObject(Item.ItemToDrop) ||
                    Item.ItemToDrop->Name.ToWString() !=
                        L"Athena_Glider_Item_BigTeamMode")
                {
                    continue;
                }

                int32 Count = 0;
                if (!TryEvaluateMutatorGrantCount(
                        Item.NumberToGive, Count))
                {
                    continue;
                }

                OutMutator = Mutator;
                OutItemDefinition = Item.ItemToDrop;
                OutCount = Count;
                return true;
            }
        }
        return false;
    }

    void GrantDiscoBigTeamGliderIfDue(
        AFortGameStateAthena* GameState,
        AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(PlayerController->WorldInventory))
        {
            return;
        }

        AFortAthenaMutator_GiveItemsAtGamePhaseStep*
            Mutator = nullptr;
        UFortWorldItemDefinition* ItemDefinition = nullptr;
        int32 Count = 0;
        if (!ResolveDiscoBigTeamGliderGrant(
                GameState,
                Mutator,
                ItemDefinition,
                Count))
        {
            return;
        }

        auto& GrantedPlayers =
            GAuthoredNativeLTMPhaseState
                .DiscoGliderGrantedPlayers;
        if (GrantedPlayers.contains(PlayerController))
            return;

        auto HasInitializedEntry =
            [&]()
            {
                auto Entry =
                    PlayerController->WorldInventory
                        ->Inventory.ReplicatedEntries.Search(
                            [&](FFortItemEntry& Candidate)
                            {
                                return Candidate
                                        .ItemDefinition ==
                                    ItemDefinition;
                            },
                            FFortItemEntry::Size());
                return Entry &&
                    HasInitializedBigTeamGliderCharges(
                        *Entry);
            };
        if (HasInitializedEntry())
        {
            // Native delivery won the race. Remember it so dropping or
            // exhausting the authored 50-use gadget cannot turn this
            // watchdog into an infinite refill.
            GrantedPlayers.insert(PlayerController);
            return;
        }

        GiveConfiguredMutatorItem(
            PlayerController,
            ItemDefinition,
            Count);
        if (HasInitializedEntry())
            GrantedPlayers.insert(PlayerController);
    }

    void ReconcileDiscoBigTeamGliders(
        AFortGameStateAthena* GameState)
    {
        AFortAthenaMutator_GiveItemsAtGamePhaseStep*
            Mutator = nullptr;
        UFortWorldItemDefinition* ItemDefinition = nullptr;
        int32 Count = 0;
        if (!ResolveDiscoBigTeamGliderGrant(
                GameState,
                Mutator,
                ItemDefinition,
                Count))
        {
            return;
        }

        auto& Players =
            Mutator->CachedGameMode->AlivePlayers;
        for (auto Candidate : Players)
        {
            auto PlayerController =
                IsLiveObject(Candidate)
                    ? Candidate
                          ->Cast<AFortPlayerControllerAthena>()
                    : nullptr;
            if (IsLiveObject(PlayerController) &&
                IsLiveObject(
                    PlayerController->WorldInventory))
            {
                GrantDiscoBigTeamGliderIfDue(
                    GameState,
                    PlayerController);
            }
        }
    }
}

void FFortAthenaNativeLTMCompatibility::
    HandleDiscoPlayerReady(
        AFortPlayerControllerAthena* PlayerController)
{
    UWorld* World = UWorld::GetWorld();
    auto GameState =
        World && World->GameState
            ? World->GameState->Cast<AFortGameStateAthena>()
            : nullptr;
    if (!IsSupportedBuild() ||
        !World ||
        GNativeLTMCompatibilityState.World != World ||
        !IsNativeDiscoDescriptor(
            GNativeLTMCompatibilityState.Descriptor))
    {
        return;
    }

    GrantDiscoBigTeamGliderIfDue(
        GameState, PlayerController);
}

void AFortAthenaMutator_GiveItemsAtGamePhaseStep::OnGamePhaseStepChanged(UObject* Context, FFrame& Stack)
{
    auto CallOriginal = [&]()
    {
        if (OnGamePhaseStepChangedOG)
            OnGamePhaseStepChangedOG(Context, Stack);
    };
    auto ConsumeFrame = [&]()
    {
        TScriptInterface<IInterface> SafeZoneInterface{};
        uint8 GamePhaseStep = 0;
        Stack.StepCompiledIn(&SafeZoneInterface);
        Stack.StepCompiledIn(&GamePhaseStep);
        Stack.IncrementCode();
    };

    // Wax/Bounty retains its stock grant handler. Food Fight uses the
    // compatibility grant because the stripped server omits the stock phase
    // body and needs the glider's tracked CurrentCharges seeded explicitly.
    if (OnGamePhaseStepChangedOG &&
        ShouldUseNativeWaxConfiguredItemGrant())
    {
        CallOriginal();
        return;
    }

    const UClass* MutatorClass = StaticClass();
    auto Mutator =
        IsLiveObject(Context) &&
                MutatorClass &&
                Context->IsA(MutatorClass)
            ? static_cast<
                  AFortAthenaMutator_GiveItemsAtGamePhaseStep*>(
                  Context)
            : nullptr;
    int32 EntrySize = 0;
    if (!Mutator ||
        !Mutator->HasPhaseToGiveItems() ||
        !Mutator->HasItemsToGive() ||
        !ResolveMutatorGrantLayout(
            Mutator,
            Mutator->ItemsToGive,
            EntrySize))
    {
        if (OnGamePhaseStepChangedOG)
            CallOriginal();
        else
            ConsumeFrame();
        return;
    }

    TScriptInterface<IInterface> SafeZoneInterface{};
    uint8 GamePhaseStep = 0;
    Stack.StepCompiledIn(&SafeZoneInterface);
    Stack.StepCompiledIn(&GamePhaseStep);
    Stack.IncrementCode();
    if (GamePhaseStep == Mutator->PhaseToGiveItems)
    {
        GrantReflectedMutatorItems(
            Mutator,
            Mutator->ItemsToGive,
            EntrySize);
    }
}

void AFortAthenaMutator_GiveItemsAtGamePhase::OnGamePhaseChanged(UObject* Context, FFrame& Stack)
{
    auto CallOriginal = [&]()
    {
        if (OnGamePhaseChangedOG)
            OnGamePhaseChangedOG(Context, Stack);
    };
    auto ConsumeFrame = [&]()
    {
        uint8 GamePhase = 0;
        Stack.StepCompiledIn(&GamePhase);
        Stack.IncrementCode();
    };

    if (OnGamePhaseChangedOG &&
        ShouldUseNativeWaxConfiguredItemGrant())
    {
        CallOriginal();
        return;
    }

    const UClass* MutatorClass = StaticClass();
    auto Mutator =
        IsLiveObject(Context) &&
                MutatorClass &&
                Context->IsA(MutatorClass)
            ? static_cast<
                  AFortAthenaMutator_GiveItemsAtGamePhase*>(
                  Context)
            : nullptr;
    int32 EntrySize = 0;
    if (!Mutator ||
        !Mutator->HasPhaseToGiveItems() ||
        !Mutator->HasItemsToGive() ||
        !ResolveMutatorGrantLayout(
            Mutator,
            Mutator->ItemsToGive,
            EntrySize))
    {
        if (OnGamePhaseChangedOG)
            CallOriginal();
        else
            ConsumeFrame();
        return;
    }

    uint8 GamePhase = 0;
    Stack.StepCompiledIn(&GamePhase);
    Stack.IncrementCode();
    if (GamePhase == Mutator->PhaseToGiveItems)
    {
        GrantReflectedMutatorItems(
            Mutator,
            Mutator->ItemsToGive,
            EntrySize);
    }
}

void AFortAthenaMutator_Wax::PostLoadHook()
{
    GWaxCommonDeadPawnHookInstalled = false;
    if (!FFortAthenaNativeLTMCompatibility::IsSupportedBuild() ||
        !StaticClass())
    {
        return;
    }

    const auto DefaultObject = GetDefaultObj();
    UFunction* DeathFunction =
        DefaultObject
            ? DefaultObject->GetFunction("CommonDeadPawn")
            : nullptr;
    if (!DeathFunction || !DeathFunction->ExecFunction)
    {
        SDK::DbgLog(
            "[Wax1040] native CommonDeadPawn implementation "
            "was not available for observation\n");
        return;
    }

    void* HookAddress =
        reinterpret_cast<void*>(CommonDeadPawnHook);
    if (DeathFunction->ExecFunction == HookAddress &&
        CommonDeadPawnHookOG)
    {
        GWaxCommonDeadPawnHookInstalled = true;
        return;
    }

    Utils::ExecHook(
        DeathFunction,
        CommonDeadPawnHook,
        CommonDeadPawnHookOG);
    GWaxCommonDeadPawnHookInstalled =
        CommonDeadPawnHookOG != nullptr &&
        DeathFunction->ExecFunction == HookAddress;
    SDK::DbgLog(
        "[Wax1040] CommonDeadPawn exact-once observer "
        "installed=%d\n",
        GWaxCommonDeadPawnHookInstalled ? 1 : 0);
}

void AFortAthenaMutator_Heist::PostLoadHook()
{
    if (!FFortAthenaNativeLTMCompatibility::IsSupportedBuild() ||
        !StaticClass())
    {
        return;
    }

    InstallNative1040ExitCraftTimerPatch();

    const auto DefaultObject = GetDefaultObj();
    UFunction* PhaseFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                "OnGamePhaseStepChanged")
            : nullptr;
    uint32 ParametersSize = 0;
    uint32 SafeZoneInterfaceOffset = 0;
    uint32 GamePhaseStepOffset = 0;
    if (!PhaseFunction || !PhaseFunction->ExecFunction ||
        !ResolveNative1040PhaseLayout(
            PhaseFunction,
            ParametersSize,
            SafeZoneInterfaceOffset,
            GamePhaseStepOffset))
    {
        SDK::DbgLog(
            "[Getaway1040] native Heist phase implementation "
            "was not available for observation\n");
        return;
    }

    Utils::ExecHook(
        PhaseFunction,
        OnGamePhaseStepChanged,
        OnGamePhaseStepChangedOG);
    SDK::DbgLog(
        "[Getaway1040] installed native Heist phase observer\n");
}

void AFortAthenaMutator_Ashton::PostLoadHook()
{
    if (!FFortAthenaNativeLTMCompatibility::IsSupportedBuild() ||
        !StaticClass())
    {
        return;
    }

    const auto DefaultObject = GetDefaultObj();
    UFunction* PhaseFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                "OnGamePhaseStepChanged")
            : nullptr;
    uint32 ParametersSize = 0;
    uint32 SafeZoneInterfaceOffset = 0;
    uint32 GamePhaseStepOffset = 0;
    if (PhaseFunction &&
        PhaseFunction->ExecFunction &&
        ResolveNative1040PhaseLayout(
            PhaseFunction,
            ParametersSize,
            SafeZoneInterfaceOffset,
            GamePhaseStepOffset))
    {
        void* HookAddress =
            reinterpret_cast<void*>(
                OnGamePhaseStepChanged);
        if (PhaseFunction->ExecFunction != HookAddress)
        {
            Utils::ExecHook(
                PhaseFunction,
                OnGamePhaseStepChanged,
                OnGamePhaseStepChangedOG);
        }
        SDK::DbgLog(
            "[Ashton1040] installed native phase-step "
            "observer\n");
    }
    else
    {
        SDK::DbgLog(
            "[Ashton1040] native phase-step implementation "
            "was not available for observation\n");
    }

    UFunction* PickupDestroyFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                  "OnPickupDestroying")
            : nullptr;
    if (PickupDestroyFunction &&
        PickupDestroyFunction->ExecFunction)
    {
        void* HookAddress =
            reinterpret_cast<void*>(
                OnPickupDestroying);
        if (PickupDestroyFunction
                ->ExecFunction != HookAddress)
        {
            Utils::ExecHook(
                PickupDestroyFunction,
                OnPickupDestroying,
                OnPickupDestroyingOG);
        }
        SDK::DbgLog(
            "[Ashton1040] installed native stone-destroy "
            "observer\n");
    }
    else
    {
        SDK::DbgLog(
            "[Ashton1040] native stone-destroy "
            "implementation was not available for "
            "observation\n");
    }

    UFunction* SelectLeaderFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                  "SelectNextVillainLeader")
            : nullptr;
    if (SelectLeaderFunction &&
        SelectLeaderFunction->ExecFunction)
    {
        void* HookAddress =
            reinterpret_cast<void*>(
                SelectNextVillainLeader);
        if (SelectLeaderFunction->ExecFunction !=
            HookAddress)
        {
            Utils::ExecHook(
                SelectLeaderFunction,
                SelectNextVillainLeader,
                SelectNextVillainLeaderOG);
        }
        SDK::DbgLog(
            "[Ashton1040] installed native leader-selection "
            "guard\n");
    }
    else
    {
        SDK::DbgLog(
            "[Ashton1040] native leader-selection "
            "implementation was not available for "
            "observation\n");
    }
}

void AFortAthenaMutator_Disco::PostLoadHook()
{
    if (!FFortAthenaNativeLTMCompatibility::IsSupportedBuild() ||
        !StaticClass())
    {
        return;
    }

    const auto DefaultObject = GetDefaultObj();
    UFunction* PhaseFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                "OnGamePhaseChanged")
            : nullptr;
    if (PhaseFunction && PhaseFunction->ExecFunction &&
        HasSingleByteInputParameter(PhaseFunction))
    {
        void* HookAddress =
            reinterpret_cast<void*>(OnGamePhaseChanged);
        if (PhaseFunction->ExecFunction != HookAddress)
        {
            Utils::ExecHook(
                PhaseFunction,
                OnGamePhaseChanged,
                OnGamePhaseChangedOG);
        }
        SDK::DbgLog(
            "[Disco1040] installed native game-phase observer\n");
    }
    else
    {
        SDK::DbgLog(
            "[Disco1040] native game-phase implementation "
            "was not available for observation\n");
    }

    UFunction* PhaseStepFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                "OnGamePhaseStepChanged")
            : nullptr;
    uint32 ParametersSize = 0;
    uint32 SafeZoneInterfaceOffset = 0;
    uint32 GamePhaseStepOffset = 0;
    if (!PhaseStepFunction ||
        !PhaseStepFunction->ExecFunction ||
        !ResolveNative1040PhaseLayout(
            PhaseStepFunction,
            ParametersSize,
            SafeZoneInterfaceOffset,
            GamePhaseStepOffset))
    {
        SDK::DbgLog(
            "[Disco1040] native phase-step implementation "
            "was not available for observation\n");
        return;
    }

    void* HookAddress =
        reinterpret_cast<void*>(OnGamePhaseStepChanged);
    if (PhaseStepFunction->ExecFunction != HookAddress)
    {
        Utils::ExecHook(
            PhaseStepFunction,
            OnGamePhaseStepChanged,
            OnGamePhaseStepChangedOG);
    }
    SDK::DbgLog(
        "[Disco1040] installed native phase-step observer\n");
}

void AFortAthenaMutator_Barrier::PostLoadHook()
{
    if (!FFortAthenaNativeLTMCompatibility::IsSupportedBuild() ||
        !StaticClass())
    {
        return;
    }

    const auto DefaultObject = GetDefaultObj();
    UFunction* PhaseFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                "OnGamePhaseStepChanged")
            : nullptr;
    if (!PhaseFunction || !PhaseFunction->ExecFunction)
    {
        SDK::DbgLog(
            "[FoodFight] native Barrier phase implementation was not "
            "available for hooking\n");
        return;
    }

    Utils::ExecHook(
        PhaseFunction,
        OnGamePhaseStepChanged,
        OnGamePhaseStepChangedOG);
    SDK::DbgLog(
        "[FoodFight] installed native Barrier BusLocked "
        "bootstrap\n");
}

void AFortAthenaMutator_GiveItemsAtGamePhaseStep::PostLoadHook()
{
    const auto DefaultObject = GetDefaultObj();
    UFunction* PhaseFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                  "OnGamePhaseStepChanged")
            : nullptr;
    if (!PhaseFunction || !PhaseFunction->ExecFunction)
        return;

    void* HookAddress =
        reinterpret_cast<void*>(
            OnGamePhaseStepChanged);
    if (PhaseFunction->ExecFunction == HookAddress)
        return;

    Utils::ExecHook(
        PhaseFunction,
        OnGamePhaseStepChanged,
        OnGamePhaseStepChangedOG);
}

void AFortAthenaMutator_GiveItemsAtGamePhase::PostLoadHook()
{
    const auto DefaultObject = GetDefaultObj();
    UFunction* PhaseFunction =
        DefaultObject
            ? DefaultObject->GetFunction(
                  "OnGamePhaseChanged")
            : nullptr;
    if (!PhaseFunction || !PhaseFunction->ExecFunction)
        return;

    void* HookAddress =
        reinterpret_cast<void*>(OnGamePhaseChanged);
    if (PhaseFunction->ExecFunction == HookAddress)
        return;

    Utils::ExecHook(
        PhaseFunction,
        OnGamePhaseChanged,
        OnGamePhaseChangedOG);
}
