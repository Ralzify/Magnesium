#pragma once

#include "../../pch.h"
#include "FortSafeZoneIndicator.h"

class AFortAthenaMapInfo;
class AFortGameMode;
class UFortGameStateComponent_BattleRoyaleGamePhaseLogic;

enum class ECustomSafeZoneRuntimeFailure : uint8
{
	None,
	NotRequested,
	UnsupportedVersion,
	MissingWorld,
	MissingMapInfo,
	MissingSnapshot,
	TooFewNodes,
	TooManyNodes,
	InvalidNode,
	RadiusIncreaseNotAllowed,
	MapProjectionUnavailable,
	InsufficientPhaseCapacity,
	MissingPhaseFields,
	MissingIndicatorFields,
	MissingPhasePublisher,
	InvalidLegacyLocations,
};

struct FCustomSafeZoneRuntimeStatus
{
	ECustomSafeZoneRuntimeFailure Failure =
		ECustomSafeZoneRuntimeFailure::NotRequested;
	UWorld* World = nullptr;
	int32 RuntimeStartPhase = -1;
	int32 NodeCount = 0;
	int32 PhaseCapacity = 0;
	// Part of status identity: toggling final closure changes required native
	// capacity without changing the authored/numbered circle count.
	bool bCloseFinalCircle = false;
	// Zero-based authored edge for failures tied to one transition. A value of
	// -1 means the failure is not edge-specific.
	int32 OffendingEdge = -1;
	// True only while ReadyToStartMatch is deliberately held before listen.
	// The GUI may correct this rejected draft; failures after acceptance remain
	// immutable for the lifetime of the match.
	bool bCanCorrectBeforeListen = false;
};

enum class ECustomSafeZonePreflightResult : uint8
{
	Ready,
	Pending,
	Invalid,
};

enum class ECustomSafeZoneLegacyPhaseOwnerPath : uint8
{
	Unavailable,
	NativePhaseHook,
	DeadlineFallback,
};

// Game-thread-only runtime for the immutable moving safe-zone snapshot. The
// GUI publishes configuration; this layer owns validation, per-world mapping,
// and phase-boundary publication into Fortnite's versioned storm owners.
namespace CustomSafeZoneRuntime
{
	// Clears match-owned compiled/activation state even when a native restart
	// reuses the same UWorld, GameMode, and GameState objects.
	void ResetForMatch(UWorld* World);
	// True while a same-world restart is held in pregame until its next exact
	// immutable sequence has passed the ordinary capability preflight. The
	// owner check prevents a rejected match from holding a replacement world or
	// GameMode generation that happens to start before the old NetDriver exits.
	bool IsMatchRestartPreflightPending(UWorld* World);

	void SetAuthoritativePhasePublisherAvailable(bool bAvailable);
	bool IsAuthoritativePhasePublisherCapabilityKnown();
	bool IsAuthoritativePhasePublisherAvailable();
	void SetLegacyPhaseOwnerPath(
		ECustomSafeZoneLegacyPhaseOwnerPath OwnerPath);
	ECustomSafeZoneLegacyPhaseOwnerPath GetLegacyPhaseOwnerPath();
	// Safely resolves only a live FN25.20+ component owned by the current
	// GameState; reflected Get results are verified and have a bounded component
	// fallback for builds whose helper returns a stale object.
	UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
		ResolveLiveComponentPhaseLogic(UWorld* World);

	bool IsMovingModeRequested();
	int32 ResolveRuntimeStartPhase();
	int32 ResolveNativePhaseCapacity(AFortAthenaMapInfo* MapInfo);
	// Final closure contributes an implicit radius-zero target and therefore
	// consumes one more native phase than the authored circle count alone.
	bool DoesSequenceFitPhaseCapacity(
		int32 NodeCount,
		int32 PhaseCapacity,
		bool bCloseFinalCircle = false);
	ECustomSafeZonePreflightResult PreflightForServerStart(
		UWorld* World,
		AFortGameMode* GameMode,
		AFortAthenaMapInfo* MapInfo,
		int32 PhaseCapacity,
		bool bTreatUnavailableAsInvalid = false,
		bool bCanCorrectBeforeListen = true);

	bool ApplyToPhaseArray(
		UWorld* World,
		AFortAthenaMapInfo* MapInfo,
		AFortSafeZoneIndicator* Indicator,
		TArray<FFortSafeZonePhaseInfo>& Phases,
		const char* Source,
		bool bStableProcessArray = false);

	bool ApplyToLegacyLocations(
		AFortGameMode* GameMode,
		AFortAthenaMapInfo* MapInfo,
		int32 RequiredLocationCount,
		bool bHasNativePhasePublisher);

	bool ApplyNativePhase(
		AFortGameMode* GameMode,
		AFortAthenaMapInfo* MapInfo,
		int32 ActivePhase,
		float TimeSeconds,
		const char* Source,
		bool bHasNativePhasePublisher = true);

	bool PublishManagedPhase(
		UWorld* World,
		AFortAthenaMapInfo* MapInfo,
		AFortSafeZoneIndicator* Indicator,
		int32 ActivePhase,
		const char* Source,
		TArray<FFortSafeZonePhaseInfo>* ManagedPhases = nullptr,
		bool bStableProcessArray = false);

	bool TryGetSourceCircle(
		UWorld* World,
		AFortAthenaMapInfo* MapInfo,
		FVector& OutCenter,
		float& OutRadius,
		int32 PhaseCapacityHint = 0,
		bool bHasNativePhasePublisher = true);
	bool IsMovingPlanActive(
		UWorld* World,
		AFortSafeZoneIndicator* Indicator = nullptr);

	FCustomSafeZoneRuntimeStatus GetStatus();
	const char* GetFailureName(
		ECustomSafeZoneRuntimeFailure Failure);

#if defined(_DEBUG)
	void RunMappingSelfTests();
#endif
}
