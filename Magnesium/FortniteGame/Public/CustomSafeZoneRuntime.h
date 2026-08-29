#pragma once

#include "../../pch.h"
#include "FortSafeZoneIndicator.h"

class AFortAthenaMapInfo;
class AFortGameMode;
class UFortGameStateComponent_BattleRoyaleGamePhaseLogic;

enum class ECustomSafeZoneRuntimeFailure : uint8
{
    None, NotRequested, UnsupportedVersion, MissingWorld, MissingMapInfo, MissingSnapshot,
    TooFewNodes, TooManyNodes, InvalidNode, RadiusIncreaseNotAllowed, MapProjectionUnavailable,
    InsufficientPhaseCapacity, MissingPhaseFields, MissingIndicatorFields, MissingPhasePublisher,
    InvalidLegacyLocations,
};

struct FCustomSafeZoneRuntimeStatus
{
    ECustomSafeZoneRuntimeFailure Failure = ECustomSafeZoneRuntimeFailure::NotRequested;
    UWorld* World = nullptr;
    int32 RuntimeStartPhase = -1;
    int32 NodeCount = 0;
    int32 PhaseCapacity = 0;
    bool bCloseFinalCircle = false;
    int32 OffendingEdge = -1;
    bool bCanCorrectBeforeListen = false;
};

enum class ECustomSafeZonePreflightResult : uint8
{
    Ready, Pending, Invalid,
};

enum class ECustomSafeZoneLegacyPhaseOwnerPath : uint8
{
    Unavailable, NativePhaseHook, DeadlineFallback,
};

namespace CustomSafeZoneRuntime
{
    void ResetForMatch(UWorld* World);
    bool IsMatchRestartPreflightPending(UWorld* World);

    void SetAuthoritativePhasePublisherAvailable(bool bAvailable);
    bool IsAuthoritativePhasePublisherCapabilityKnown();
    bool IsAuthoritativePhasePublisherAvailable();
    void SetLegacyPhaseOwnerPath(ECustomSafeZoneLegacyPhaseOwnerPath OwnerPath);
    ECustomSafeZoneLegacyPhaseOwnerPath GetLegacyPhaseOwnerPath();
    UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
        ResolveLiveComponentPhaseLogic(UWorld* World);

    bool IsMovingModeRequested();
    int32 ResolveRuntimeStartPhase();
    int32 ResolveNativePhaseCapacity(AFortAthenaMapInfo* MapInfo);
    bool DoesSequenceFitPhaseCapacity(int32 NodeCount, int32 PhaseCapacity,
        bool bCloseFinalCircle = false);
    ECustomSafeZonePreflightResult PreflightForServerStart(UWorld* World, AFortGameMode* GameMode,
        AFortAthenaMapInfo* MapInfo, int32 PhaseCapacity, bool bTreatUnavailableAsInvalid = false,
        bool bCanCorrectBeforeListen = true);

    bool ApplyToPhaseArray(UWorld* World, AFortAthenaMapInfo* MapInfo,
        AFortSafeZoneIndicator* Indicator, TArray<FFortSafeZonePhaseInfo>& Phases,
        const char* Source, bool bStableProcessArray = false);

    bool ApplyToLegacyLocations(AFortGameMode* GameMode, AFortAthenaMapInfo* MapInfo,
        int32 RequiredLocationCount, bool bHasNativePhasePublisher);

    bool ApplyNativePhase(AFortGameMode* GameMode, AFortAthenaMapInfo* MapInfo, int32 ActivePhase,
        float TimeSeconds, const char* Source, bool bHasNativePhasePublisher = true);

    bool PublishManagedPhase(UWorld* World, AFortAthenaMapInfo* MapInfo,
        AFortSafeZoneIndicator* Indicator, int32 ActivePhase, const char* Source,
        TArray<FFortSafeZonePhaseInfo>* ManagedPhases = nullptr, bool bStableProcessArray = false);

    bool TryGetSourceCircle(UWorld* World, AFortAthenaMapInfo* MapInfo, FVector& OutCenter,
        float& OutRadius, int32 PhaseCapacityHint = 0, bool bHasNativePhasePublisher = true);
    bool IsMovingPlanActive(UWorld* World, AFortSafeZoneIndicator* Indicator = nullptr);

    FCustomSafeZoneRuntimeStatus GetStatus();
    const char* GetFailureName(ECustomSafeZoneRuntimeFailure Failure);

#if defined(_DEBUG)
    void RunMappingSelfTests();
#endif
}
