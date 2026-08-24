#include "pch.h"
#include "../Public/CustomSafeZoneRuntime.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortGameMode.h"
#include "../Public/FortGameStateAthena.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Public/GUI.h"
#include "../../Erbium/Support/Public/VersionFeatureAdapter.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace
{
	struct FCompiledSafeZoneNode
	{
		FVector Center{};
		float Radius = 0.f;
		std::optional<float> HoldBeforeNextSeconds;
		std::optional<float> MoveToNextSeconds;
	};

	struct FCompiledCustomSafeZonePlan
	{
		int32 RuntimeStartPhase = -1;
		int32 SourcePhase = -1;
		int32 PhaseCapacity = 0;
		int32 AuthoredNodeCount = 0;
		bool bCloseFinalCircle = false;
		std::vector<FCompiledSafeZoneNode> Nodes;
	};

	struct FCapturedNativePhase
	{
		double CenterX = 0.0;
		double CenterY = 0.0;
		double CenterZ = 0.0;
		float Radius = 0.f;
		float WaitTime = 0.f;
		float ShrinkTime = 0.f;
	};

	struct FCapturedManagedPhaseArray
	{
		TArray<FFortSafeZonePhaseInfo>* Array = nullptr;
		const void* Data = nullptr;
		TWeakObjectPtr<AFortSafeZoneIndicator> Owner;
		bool bStableProcessArray = false;
		bool bHasWaitTime = false;
		bool bHasShrinkTime = false;
		std::vector<FCapturedNativePhase> Phases;
	};

	struct FCapturedLegacyCenter
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
	};

	struct FCustomSafeZoneWorldState
	{
		UWorld* World = nullptr;
		AFortGameMode* GameMode = nullptr;
		AFortGameStateAthena* GameState = nullptr;
		AFortAthenaMapInfo* MapInfo = nullptr;
		std::shared_ptr<const FCustomSafeZoneSequence> Snapshot;
		int32 CacheStartPhase = -1;
		int32 CachePhaseCapacity = 0;
		std::shared_ptr<const FCompiledCustomSafeZonePlan> Plan;
		FCustomSafeZoneRuntimeStatus Status{};
		bool bFailureLogged = false;
		bool bPhaseArrayLogged = false;
		bool bLegacyLocationsLogged = false;
		bool bPlanCommitted = false;
		bool bPreflightAccepted = false;
		// Same-world restarts retain networking, so no second listen gate exists.
		// Until lifecycle preflight accepts the new match snapshot, every runtime
		// application seam must fail closed instead of compiling opportunistically.
		bool bRequiresPreflightBeforeApply = false;
		bool bPreflightValidationInProgress = false;
		bool bLegacyLocationsCommitted = false;
		int32 LegacyCommittedStartPhase = -1;
		int32 LegacyCommittedLocationCount = 0;
		std::vector<FCapturedManagedPhaseArray>
			CapturedManagedPhaseArrays;
		bool bLegacyLocationBaselineCaptured = false;
		bool bLegacyHadInitializedProperty = false;
		bool bLegacyWasInitialized = false;
		std::vector<FCapturedLegacyCenter> LegacyLocationBaseline;
		bool bActivated = false;
		bool bPhysicalPublished = false;
		AFortSafeZoneIndicator* PublishedIndicator = nullptr;
		bool bActivationFailed = false;
		int32 LastPublishedPhase = INT32_MIN;
	};

	FCustomSafeZoneWorldState GCustomSafeZoneWorldState{};

	struct FAtomicCustomSafeZoneStatus
	{
		std::atomic<uint64> Sequence{ 0 };
		std::atomic<ECustomSafeZoneRuntimeFailure> Failure{
			ECustomSafeZoneRuntimeFailure::NotRequested
		};
		std::atomic<UWorld*> World{ nullptr };
		std::atomic<int32> RuntimeStartPhase{ -1 };
		std::atomic<int32> NodeCount{ 0 };
		std::atomic<int32> PhaseCapacity{ 0 };
		std::atomic_bool CloseFinalCircle{ false };
		std::atomic<int32> OffendingEdge{ -1 };
		std::atomic_bool CanCorrectBeforeListen{ false };
	};

	FAtomicCustomSafeZoneStatus GVisibleCustomSafeZoneStatus{};
	// -1 means hook discovery has not run, 0 unavailable, 1 available.
	std::atomic<int32> GAuthoritativePhasePublisherCapability{ -1 };
	std::atomic<ECustomSafeZoneLegacyPhaseOwnerPath>
		GLegacyPhaseOwnerPath{
			ECustomSafeZoneLegacyPhaseOwnerPath::Unavailable
		};
	std::atomic_bool GMatchRestartPreflightPending{ false };
	std::atomic<UWorld*> GMatchRestartPreflightWorld{ nullptr };
	std::atomic<AFortGameMode*> GMatchRestartPreflightGameMode{ nullptr };
	std::atomic<AFortGameStateAthena*> GMatchRestartPreflightGameState{
		nullptr
	};

	void SetMatchRestartPreflightPending(UWorld* World, bool bPending)
	{
		if (bPending && World)
		{
			GMatchRestartPreflightGameMode.store(
				(AFortGameMode*)World->AuthorityGameMode,
				std::memory_order_relaxed);
			GMatchRestartPreflightGameState.store(
				(AFortGameStateAthena*)World->GameState,
				std::memory_order_relaxed);
			GMatchRestartPreflightWorld.store(
				World, std::memory_order_relaxed);
			GMatchRestartPreflightPending.store(
				true, std::memory_order_release);
			return;
		}

		GMatchRestartPreflightPending.store(
			false, std::memory_order_release);
		GMatchRestartPreflightWorld.store(
			nullptr, std::memory_order_relaxed);
		GMatchRestartPreflightGameMode.store(
			nullptr, std::memory_order_relaxed);
		GMatchRestartPreflightGameState.store(
			nullptr, std::memory_order_relaxed);
	}

	void PublishVisibleStatus(
		const FCustomSafeZoneRuntimeStatus& Status)
	{
		auto& Visible = GVisibleCustomSafeZoneStatus;
		Visible.Sequence.fetch_add(1, std::memory_order_acq_rel);
		Visible.Failure.store(Status.Failure, std::memory_order_relaxed);
		Visible.World.store(Status.World, std::memory_order_relaxed);
		Visible.RuntimeStartPhase.store(
			Status.RuntimeStartPhase, std::memory_order_relaxed);
		Visible.NodeCount.store(
			Status.NodeCount, std::memory_order_relaxed);
		Visible.PhaseCapacity.store(
			Status.PhaseCapacity, std::memory_order_relaxed);
		Visible.CloseFinalCircle.store(
			Status.bCloseFinalCircle, std::memory_order_relaxed);
		Visible.OffendingEdge.store(
			Status.OffendingEdge, std::memory_order_relaxed);
		Visible.CanCorrectBeforeListen.store(
			Status.bCanCorrectBeforeListen,
			std::memory_order_relaxed);
		Visible.Sequence.fetch_add(1, std::memory_order_release);
	}

	bool IsFiniteVector(const FVector& Value)
	{
		return std::isfinite(Value.X) &&
			std::isfinite(Value.Y) &&
			std::isfinite(Value.Z);
	}

	bool IsSupportedMovingZoneVersion()
	{
		return VersionInfo.FortniteVersion >= 0.f &&
			VersionInfo.FortniteVersion < 31.f;
	}

	bool IsMovingConfigurationEnabled(
		const std::shared_ptr<const FCustomSafeZoneSequence>& Snapshot)
	{
		return FConfiguration::bLateGame.load(
				std::memory_order_acquire) &&
			FConfiguration::bCustomSafeZone.load(
				std::memory_order_acquire) &&
			Snapshot && Snapshot->bMovingZoneEnabled;
	}

	int32 EffectiveNodeCount(
		int32 AuthoredNodeCount,
		bool bCloseFinalCircle)
	{
		if (AuthoredNodeCount <= 0)
			return AuthoredNodeCount;
		return AuthoredNodeCount + (bCloseFinalCircle ? 1 : 0);
	}

	int32 EffectiveNodeCount(const FCustomSafeZoneSequence& Sequence)
	{
		return EffectiveNodeCount(
			(int32)Sequence.Nodes.size(),
			Sequence.bCloseFinalCircle);
	}

	bool HasMovingTransition(
		const std::shared_ptr<const FCustomSafeZoneSequence>& Snapshot)
	{
		return Snapshot && EffectiveNodeCount(*Snapshot) >= 2;
	}

	int32 LogicalToRuntimeStartPhase(
		int32 LogicalPhase, double FortniteVersion)
	{
		return LogicalPhase + (FortniteVersion >= 24.f ? 3 : 0);
	}

	float ResolveAuthoredDuration(
		const std::optional<float>& Authored,
		float NativeDuration)
	{
		return Authored.value_or(NativeDuration);
	}

	int32 GeometryNodeIndexForPhase(
		int32 SourcePhase, int32 NodeCount, int32 Phase)
	{
		if (NodeCount <= 0 || Phase < SourcePhase)
			return -1;

		return (std::min)(Phase - SourcePhase, NodeCount - 1);
	}

	int32 TimingSourceNodeIndexForPhase(
		int32 RuntimeStartPhase, int32 NodeCount, int32 Phase)
	{
		const int32 EdgeIndex = Phase - RuntimeStartPhase;
		if (NodeCount < 2 || EdgeIndex < 0 ||
			EdgeIndex >= NodeCount - 1)
			return -1;

		return EdgeIndex;
	}

	bool HasCapacityForPlan(
		int32 RuntimeStartPhase,
		int32 NodeCount,
		int32 PhaseCapacity)
	{
		if (RuntimeStartPhase < 1 || NodeCount < 2 ||
			PhaseCapacity <= 0)
		{
			return false;
		}

		const int64 LastDistinctPhase =
			(int64)RuntimeStartPhase + NodeCount - 2;
		return LastDistinctPhase >= 0 &&
			LastDistinctPhase < PhaseCapacity;
	}

	int32 ResolvePhaseCapacity(
		AFortAthenaMapInfo* MapInfo,
		int32 Hint)
	{
		if (Hint > 0 && Hint <= 64)
			return Hint;

		if (!MapInfo)
		{
			return 0;
		}

		// Early Athena maps publish one native definition per phase instead of
		// the later singular curve definition. Their locations array can still be
		// empty at pre-listen, so this is the only exact capacity evidence there.
		if (MapInfo->HasSafeZoneDefinitions())
		{
			const int32 DefinitionCount =
				MapInfo->SafeZoneDefinitions.Num();
			if (DefinitionCount > 0 && DefinitionCount <= 64 &&
				MapInfo->SafeZoneDefinitions.Max() >= DefinitionCount &&
				MapInfo->SafeZoneDefinitions.Max() <= 128)
			{
				return DefinitionCount;
			}
		}

		if (!MapInfo->HasSafeZoneDefinition() ||
			!FFortSafeZoneDefinition::HasCount())
		{
			return 0;
		}

		const float EvaluatedCount =
			MapInfo->SafeZoneDefinition.Count.Evaluate();
		const float RoundedCount = std::round(EvaluatedCount);
		if (!std::isfinite(EvaluatedCount) ||
			std::abs(EvaluatedCount - RoundedCount) > 0.01f ||
			RoundedCount < 1.f || RoundedCount > 64.f)
		{
			return 0;
		}
		return (int32)RoundedCount;
	}

	void ResetStateForWorld(UWorld* World)
	{
		auto GameMode = World
			? (AFortGameMode*)World->AuthorityGameMode
			: nullptr;
		auto GameState = World
			? (AFortGameStateAthena*)World->GameState
			: nullptr;
		if (GCustomSafeZoneWorldState.World == World &&
			GCustomSafeZoneWorldState.GameMode == GameMode &&
			GCustomSafeZoneWorldState.GameState == GameState)
		{
			return;
		}

		FConfiguration::ReleaseCustomSafeZoneSequenceForMatch();
		SetMatchRestartPreflightPending(nullptr, false);
		GCustomSafeZoneWorldState = {};
		GCustomSafeZoneWorldState.World = World;
		GCustomSafeZoneWorldState.GameMode = GameMode;
		GCustomSafeZoneWorldState.GameState = GameState;
		GCustomSafeZoneWorldState.Status.World = World;
		GCustomSafeZoneWorldState.Status.Failure =
			ECustomSafeZoneRuntimeFailure::NotRequested;
		PublishVisibleStatus(GCustomSafeZoneWorldState.Status);
	}

	void ResetStateForCacheKey(
		AFortAthenaMapInfo* MapInfo,
		std::shared_ptr<const FCustomSafeZoneSequence> Snapshot,
		int32 RuntimeStartPhase,
		int32 PhaseCapacity)
	{
		auto& State = GCustomSafeZoneWorldState;
		if (State.MapInfo == MapInfo && State.Snapshot == Snapshot &&
			State.CacheStartPhase == RuntimeStartPhase &&
			State.CachePhaseCapacity == PhaseCapacity)
		{
			return;
		}

		State.MapInfo = MapInfo;
		State.Snapshot = std::move(Snapshot);
		State.CacheStartPhase = RuntimeStartPhase;
		State.CachePhaseCapacity = PhaseCapacity;
		State.Plan.reset();
		State.Status = {};
		State.Status.World = State.World;
		State.Status.Failure =
			ECustomSafeZoneRuntimeFailure::NotRequested;
		State.bFailureLogged = false;
		State.bPhaseArrayLogged = false;
		State.bLegacyLocationsLogged = false;
		State.bPlanCommitted = false;
		State.bPreflightAccepted = false;
		State.bLegacyLocationsCommitted = false;
		State.LegacyCommittedStartPhase = -1;
		State.LegacyCommittedLocationCount = 0;
		State.bActivated = false;
		State.bPhysicalPublished = false;
		State.PublishedIndicator = nullptr;
		State.bActivationFailed = false;
		State.LastPublishedPhase = INT32_MIN;
		PublishVisibleStatus(State.Status);
	}

	void SetFailure(
		ECustomSafeZoneRuntimeFailure Failure,
		int32 RuntimeStartPhase,
		int32 NodeCount,
		int32 PhaseCapacity,
		const char* Detail,
		int32 OffendingEdge = -1)
	{
		auto& State = GCustomSafeZoneWorldState;
		// Legacy builds need their center array before the indicator is spawned.
		// If the later indicator/timing preflight fails, flatten only the range
		// authored by this plan back to node one before stationary fallback runs.
		if (State.bLegacyLocationsCommitted && State.Plan &&
			!State.Plan->Nodes.empty() && State.GameMode &&
			State.GameMode->HasSafeZoneLocations())
		{
			auto& Locations = State.GameMode->SafeZoneLocations;
			const int32 Count = Locations.Num();
			const int32 VectorSize = FVector::Size();
			const int32 Begin = (std::max)(
				0, State.LegacyCommittedStartPhase);
			const int32 End = (std::min)(
				Count, State.LegacyCommittedLocationCount);
			if (Begin < End && VectorSize > 0 && VectorSize <= 0x40 &&
				Locations.Max() >= Count && Locations.Max() <= 128 &&
				SDK::MemReadable(
					Locations.GetData(),
					(size_t)Count * VectorSize))
			{
				const FVector FirstCenter(
					State.Plan->Nodes.front().Center);
				for (int32 Index = Begin; Index < End; ++Index)
				{
					auto& Location = Locations.Get(Index, VectorSize);
					Location.X = FirstCenter.X;
					Location.Y = FirstCenter.Y;
					Location.Z = FirstCenter.Z;
				}
				if (State.GameMode->HasbSafeZoneLocationsInitialized())
					State.GameMode->bSafeZoneLocationsInitialized = true;
				SDK::DbgLog(
					"[CustomSafeZonePlan] rolled back legacy locations "
					"range=[%d,%d) to node one after activation failure\n",
					Begin, End);
			}
		}
		State.Plan.reset();
		State.bPlanCommitted = false;
		// Once pre-listen validation accepts a snapshot, keep that immutable
		// cache key even if a later owner/indicator check fails. The match stays
		// fail-closed on the accepted draft and can never compile a newer GUI or
		// automatic reprojection publication after networking has begun.
		State.bLegacyLocationsCommitted = false;
		State.LegacyCommittedStartPhase = -1;
		State.LegacyCommittedLocationCount = 0;
		State.bActivated = false;
		State.bPhysicalPublished = false;
		State.PublishedIndicator = nullptr;
		State.bActivationFailed = true;
		State.Status.Failure = Failure;
		State.Status.World = State.World;
		State.Status.RuntimeStartPhase = RuntimeStartPhase;
		State.Status.NodeCount = NodeCount;
		State.Status.PhaseCapacity = PhaseCapacity;
		State.Status.bCloseFinalCircle =
			State.Snapshot && State.Snapshot->bCloseFinalCircle;
		State.Status.OffendingEdge =
			OffendingEdge >= 0 && OffendingEdge + 1 < NodeCount
				? OffendingEdge
				: -1;
		State.Status.bCanCorrectBeforeListen = false;
		PublishVisibleStatus(State.Status);

		if (!State.bFailureLogged)
		{
			State.bFailureLogged = true;
			SDK::DbgLog(
				"[CustomSafeZonePlan] status=failed code=%s "
				"version=%.2f start=%d nodes=%d capacity=%d detail=%s\n",
				CustomSafeZoneRuntime::GetFailureName(Failure),
				VersionInfo.FortniteVersion,
				RuntimeStartPhase,
				NodeCount,
				PhaseCapacity,
				Detail ? Detail : "none");
		}
	}

	void SetReadyStatus(const FCompiledCustomSafeZonePlan& Plan)
	{
		auto& State = GCustomSafeZoneWorldState;
		State.Status.Failure = ECustomSafeZoneRuntimeFailure::None;
		State.Status.World = State.World;
		State.Status.RuntimeStartPhase = Plan.RuntimeStartPhase;
		// The implicit radius-zero target is an implementation detail. Keep the
		// visible count aligned with numbered circles in the editor.
		State.Status.NodeCount = Plan.AuthoredNodeCount;
		State.Status.PhaseCapacity = Plan.PhaseCapacity;
		State.Status.bCloseFinalCircle = Plan.bCloseFinalCircle;
		State.Status.OffendingEdge = -1;
		State.Status.bCanCorrectBeforeListen = false;
		PublishVisibleStatus(State.Status);
	}

	void MarkPreflightRejected(bool bCanCorrectBeforeListen)
	{
		auto& State = GCustomSafeZoneWorldState;
		State.Status.bCanCorrectBeforeListen =
			bCanCorrectBeforeListen;
		PublishVisibleStatus(State.Status);
	}

	void MarkPlanActivated(const FCompiledCustomSafeZonePlan& Plan)
	{
		auto& State = GCustomSafeZoneWorldState;
		State.bPlanCommitted = true;
		State.bActivated = true;
		State.bActivationFailed = false;
		SetReadyStatus(Plan);
	}

	void MarkPhysicalWallPublished(
		const FCompiledCustomSafeZonePlan& Plan,
		AFortSafeZoneIndicator* Indicator)
	{
		auto& State = GCustomSafeZoneWorldState;
		State.bPhysicalPublished = true;
		State.PublishedIndicator = Indicator;
		MarkPlanActivated(Plan);
	}

	std::shared_ptr<const FCompiledCustomSafeZonePlan> GetOrCompilePlan(
		UWorld* World,
		AFortAthenaMapInfo* MapInfo,
		int32 PhaseCapacityHint)
	{
		ResetStateForWorld(World);
		auto& State = GCustomSafeZoneWorldState;
		// Once an owner has accepted and applied a plan, retain that immutable
		// snapshot/key for the match. GUI publications during setup cannot swap
		// geometry underneath an active storm.
		if (State.bPlanCommitted && State.Plan)
			return State.Plan;
		if (State.bRequiresPreflightBeforeApply &&
			!State.bPreflightAccepted &&
			!State.bPreflightValidationInProgress)
		{
			return nullptr;
		}

		const bool bUseAcceptedPreflight =
			State.bPreflightAccepted && State.Snapshot;
		const int32 RuntimeStartPhase = bUseAcceptedPreflight
			? State.CacheStartPhase
			: CustomSafeZoneRuntime::ResolveRuntimeStartPhase();
		auto Snapshot = bUseAcceptedPreflight
			? State.Snapshot
			: FConfiguration::GetCustomSafeZoneSequenceSnapshot();
		const int32 PhaseCapacity = bUseAcceptedPreflight
			? State.CachePhaseCapacity
			: ResolvePhaseCapacity(MapInfo, PhaseCapacityHint);
		if (!bUseAcceptedPreflight)
		{
			ResetStateForCacheKey(
				MapInfo, Snapshot, RuntimeStartPhase, PhaseCapacity);
		}
		if (State.bActivationFailed)
			return nullptr;

		if (!IsMovingConfigurationEnabled(Snapshot))
		{
			State.Plan.reset();
			State.Status.Failure =
				ECustomSafeZoneRuntimeFailure::NotRequested;
			State.Status.RuntimeStartPhase = RuntimeStartPhase;
			State.Status.NodeCount =
				Snapshot ? (int32)Snapshot->Nodes.size() : 0;
			State.Status.PhaseCapacity = PhaseCapacity;
			State.Status.bCloseFinalCircle =
				Snapshot && Snapshot->bCloseFinalCircle;
			State.Status.OffendingEdge = -1;
			State.Status.bCanCorrectBeforeListen = false;
			PublishVisibleStatus(State.Status);
			return nullptr;
		}

		if (!IsSupportedMovingZoneVersion())
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::UnsupportedVersion,
				RuntimeStartPhase,
				Snapshot ? (int32)Snapshot->Nodes.size() : 0,
				PhaseCapacity,
				"moving zones are supported only on FN0-30");
			return nullptr;
		}

		if (State.Plan)
			return State.Plan;

		if (!World)
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::MissingWorld,
				RuntimeStartPhase, 0, 0, "world is null");
			return nullptr;
		}
		if (!MapInfo)
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::MissingMapInfo,
				RuntimeStartPhase, 0, 0, "map info is null");
			return nullptr;
		}

		if (!Snapshot)
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::MissingSnapshot,
				RuntimeStartPhase, 0, 0,
				"configuration snapshot is null");
			return nullptr;
		}

		const int32 NodeCount = (int32)Snapshot->Nodes.size();
		const int32 CompiledNodeCount = EffectiveNodeCount(*Snapshot);
		if (CompiledNodeCount < 2)
		{
			State.Status.Failure =
				ECustomSafeZoneRuntimeFailure::NotRequested;
			State.Status.RuntimeStartPhase = RuntimeStartPhase;
			State.Status.NodeCount = NodeCount;
			State.Status.PhaseCapacity = PhaseCapacity;
			State.Status.bCloseFinalCircle =
				Snapshot->bCloseFinalCircle;
			State.Status.OffendingEdge = -1;
			State.Status.bCanCorrectBeforeListen = false;
			PublishVisibleStatus(State.Status);
			return nullptr;
		}
		if (NodeCount >
			(int32)FCustomSafeZoneSequence::MaximumNodeCount)
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::TooManyNodes,
				RuntimeStartPhase, NodeCount, 0,
				"node count exceeds schema maximum");
			return nullptr;
		}
		const auto RadiusIncreaseEdge =
			Snapshot->FindFirstRadiusIncreaseEdge();
		if (RadiusIncreaseEdge.has_value())
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::RadiusIncreaseNotAllowed,
				RuntimeStartPhase, NodeCount, PhaseCapacity,
				"a circle is larger than its preceding circle",
				(int32)*RadiusIncreaseEdge);
			return nullptr;
		}

		if (!CustomSafeZoneRuntime::
				IsAuthoritativePhasePublisherAvailable())
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::MissingPhasePublisher,
				RuntimeStartPhase, NodeCount, PhaseCapacity,
				"authoritative moving-zone owner hook is unavailable");
			return nullptr;
		}

		if (!HasCapacityForPlan(
				RuntimeStartPhase, CompiledNodeCount, PhaseCapacity))
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::
					InsufficientPhaseCapacity,
				RuntimeStartPhase, NodeCount, PhaseCapacity,
				"distinct authored nodes do not fit native phases");
			return nullptr;
		}

		auto MutablePlan =
			std::make_shared<FCompiledCustomSafeZonePlan>();
		MutablePlan->RuntimeStartPhase = RuntimeStartPhase;
		MutablePlan->SourcePhase = RuntimeStartPhase - 1;
		MutablePlan->PhaseCapacity = PhaseCapacity;
		MutablePlan->AuthoredNodeCount = NodeCount;
		MutablePlan->bCloseFinalCircle =
			Snapshot->bCloseFinalCircle;
		MutablePlan->Nodes.reserve((size_t)CompiledNodeCount);

		std::vector<FVector> ResolvedCenters;
		if (!GUI::TryResolveSafeZoneMapPoints(
				MapInfo, Snapshot->Nodes, ResolvedCenters) ||
			ResolvedCenters.size() != Snapshot->Nodes.size())
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::
					MapProjectionUnavailable,
				RuntimeStartPhase, NodeCount, PhaseCapacity,
				"normalized node batch could not be projected");
			return nullptr;
		}

		for (size_t NodeIndex = 0;
			NodeIndex < Snapshot->Nodes.size(); ++NodeIndex)
		{
			const auto& SourceNode = Snapshot->Nodes[NodeIndex];
			FCompiledSafeZoneNode Node;
			Node.Center = FVector(ResolvedCenters[NodeIndex]);

			Node.Radius = SourceNode.RadiusCm;
			Node.HoldBeforeNextSeconds =
				SourceNode.HoldBeforeNextSeconds;
			Node.MoveToNextSeconds =
				SourceNode.MoveToNextSeconds;
			const auto ValidDuration = [](const std::optional<float>& Value)
			{
				return !Value.has_value() ||
					(std::isfinite(*Value) && *Value >= 0.f &&
						*Value <= FCustomSafeZoneSequence::
							MaximumDurationSeconds);
			};
			if (!IsFiniteVector(Node.Center) ||
				!std::isfinite(Node.Radius) ||
				Node.Radius <
					FCustomSafeZoneSequence::MinimumRadiusCm ||
				Node.Radius >
					FCustomSafeZoneSequence::MaximumRadiusCm ||
				!ValidDuration(Node.HoldBeforeNextSeconds) ||
				!ValidDuration(Node.MoveToNextSeconds))
			{
				SetFailure(
					ECustomSafeZoneRuntimeFailure::InvalidNode,
					RuntimeStartPhase, NodeCount, PhaseCapacity,
					"node contains invalid geometry or timing");
				return nullptr;
			}
			MutablePlan->Nodes.push_back(std::move(Node));
		}

		if (Snapshot->bCloseFinalCircle)
		{
			// Radius zero is Fortnite's own terminal-phase representation (and
			// is accepted by SetSafeZoneRadiusAndCenter). The synthetic target
			// deliberately carries no outgoing timing; the final authored node's
			// hold/move values control the transition into this target.
			FCompiledSafeZoneNode Closure;
			Closure.Center = FVector(MutablePlan->Nodes.back().Center);
			Closure.Radius = 0.f;
			MutablePlan->Nodes.push_back(std::move(Closure));
		}
		State.Plan = MutablePlan;
		SetReadyStatus(*State.Plan);
		SDK::DbgLog(
			"[CustomSafeZonePlan] status=ready version=%.2f "
			"start=%d source=%d authoredNodes=%d compiledNodes=%d "
			"closeFinal=%d capacity=%d\n",
			VersionInfo.FortniteVersion,
			State.Plan->RuntimeStartPhase,
			State.Plan->SourcePhase,
			State.Plan->AuthoredNodeCount,
			(int32)State.Plan->Nodes.size(),
			(int)State.Plan->bCloseFinalCircle,
			State.Plan->PhaseCapacity);
		return State.Plan;
	}

	bool ValidatePhaseArray(
		TArray<FFortSafeZonePhaseInfo>& Phases)
	{
		const int32 PhaseCount = Phases.Num();
		const int32 PhaseMax = Phases.Max();
		const int32 PhaseSize = FFortSafeZonePhaseInfo::Size();
		return FFortSafeZonePhaseInfo::HasCenter() &&
			FFortSafeZonePhaseInfo::HasRadius() &&
			PhaseCount > 0 && PhaseCount <= 64 &&
			PhaseMax >= PhaseCount && PhaseMax <= 128 &&
			PhaseSize > 0 && PhaseSize <= 0x400 &&
			SDK::MemReadable(
				Phases.GetData(),
				(size_t)PhaseCount * PhaseSize);
	}

	bool IsLiveRuntimeObject(const UObject* Object)
	{
		if (!Object ||
			!SDK::MemReadable(Object, sizeof(UObject)) ||
			Object->Index < 0 ||
			Object->Index >= TUObjectArray::Num())
		{
			return false;
		}

		auto Item = TUObjectArray::GetItemByIndex(Object->Index);
		constexpr int32 InvalidObjectFlags = 0x20;
		return Item && Item->GetObject() == Object &&
			!(Item->GetFlags() & InvalidObjectFlags) &&
			Object->Class;
	}

	bool IsOwnedByCurrentGameState(
		const UObject* Object,
		const AFortGameStateAthena* GameState)
	{
		const UObject* Current = Object;
		for (int32 Depth = 0;
			Current && Depth < 16;
			++Depth)
		{
			if (Current == GameState)
				return true;
			if (!IsLiveRuntimeObject(Current))
				return false;
			Current = Current->Outer;
		}
		return false;
	}

	UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
		ResolveLiveComponentPhaseLogicInternal(UWorld* World)
	{
		if (!World || VersionInfo.FortniteVersion < 25.20f)
			return nullptr;
		auto GameState = (AFortGameStateAthena*)World->GameState;
		auto PhaseLogicClass =
			UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
				StaticClass();
		if (!IsLiveRuntimeObject(PhaseLogicClass))
			return nullptr;
		if (!UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
				Get__Initialized ||
			!UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get__Ptr)
		{
			auto DefaultObject = PhaseLogicClass->GetDefaultObj();
			if (!IsLiveRuntimeObject(DefaultObject))
				return nullptr;
			auto GetFunction = DefaultObject->GetFunction("Get");
			if (!GetFunction)
				return nullptr;
			UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
				Get__Ptr = GetFunction;
			UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
				Get__Initialized = true;
		}

		auto PhaseLogic =
			UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(
				World);
		if (!IsLiveRuntimeObject(PhaseLogic) ||
			PhaseLogic->IsDefaultObject() ||
			!IsOwnedByCurrentGameState(PhaseLogic, GameState))
		{
			PhaseLogic = IsLiveRuntimeObject(GameState)
				? (UFortGameStateComponent_BattleRoyaleGamePhaseLogic*)
					GameState->GetComponentByClass(PhaseLogicClass)
				: nullptr;
		}
		return IsLiveRuntimeObject(PhaseLogic) &&
			!PhaseLogic->IsDefaultObject() &&
			IsOwnedByCurrentGameState(PhaseLogic, GameState)
				? PhaseLogic
				: nullptr;
	}

	bool IsLiveCurrentIndicator(
		UWorld* World,
		AFortSafeZoneIndicator* Indicator)
	{
		if (!World || !IsLiveRuntimeObject(Indicator))
			return false;

		auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
		if (GameMode && GameMode->HasSafeZoneIndicator() &&
			GameMode->SafeZoneIndicator == Indicator)
		{
			return true;
		}

		auto GameState = (AFortGameStateAthena*)World->GameState;
		if (GameState && GameState->HasSafeZoneIndicator() &&
			GameState->SafeZoneIndicator == Indicator)
		{
			return true;
		}

		auto PhaseLogic =
			ResolveLiveComponentPhaseLogicInternal(World);
		return PhaseLogic &&
			PhaseLogic->HasSafeZoneIndicator() &&
			PhaseLogic->SafeZoneIndicator == Indicator;
	}

	bool CaptureManagedPhaseArray(
		AFortSafeZoneIndicator* Indicator,
		TArray<FFortSafeZonePhaseInfo>& Phases,
		bool bStableProcessArray)
	{
		if (!ValidatePhaseArray(Phases))
			return false;
		const bool bIndicatorOwned =
			Indicator && Indicator->HasSafeZonePhases() &&
			&Indicator->SafeZonePhases == &Phases;
		if (!bIndicatorOwned && !bStableProcessArray)
			return false;

		auto& State = GCustomSafeZoneWorldState;
		for (const auto& Captured :
			State.CapturedManagedPhaseArrays)
		{
			if (Captured.Array == &Phases &&
				Captured.Data == Phases.GetData() &&
				Captured.Phases.size() ==
					(size_t)Phases.Num() &&
				Captured.bStableProcessArray ==
					bStableProcessArray &&
				(bStableProcessArray ||
					Captured.Owner.Get() == Indicator))
			{
				return true;
			}
		}
		if (State.CapturedManagedPhaseArrays.size() >= 4)
			return false;

		FCapturedManagedPhaseArray Captured;
		Captured.Array = &Phases;
		Captured.Data = Phases.GetData();
		Captured.Owner = bIndicatorOwned
			? TWeakObjectPtr<AFortSafeZoneIndicator>(Indicator)
			: TWeakObjectPtr<AFortSafeZoneIndicator>{};
		Captured.bStableProcessArray = bStableProcessArray;
		Captured.bHasWaitTime =
			FFortSafeZonePhaseInfo::HasWaitTime();
		Captured.bHasShrinkTime =
			FFortSafeZonePhaseInfo::HasShrinkTime();
		Captured.Phases.reserve((size_t)Phases.Num());
		for (int32 Index = 0; Index < Phases.Num(); ++Index)
		{
			const auto& Phase = Phases.Get(
				Index, FFortSafeZonePhaseInfo::Size());
			FCapturedNativePhase Entry;
			Entry.CenterX = Phase.Center.X;
			Entry.CenterY = Phase.Center.Y;
			Entry.CenterZ = Phase.Center.Z;
			Entry.Radius = Phase.Radius;
			if (Captured.bHasWaitTime)
				Entry.WaitTime = Phase.WaitTime;
			if (Captured.bHasShrinkTime)
				Entry.ShrinkTime = Phase.ShrinkTime;
			Captured.Phases.push_back(Entry);
		}
		State.CapturedManagedPhaseArrays.push_back(
			std::move(Captured));
		return true;
	}

	void RestoreCapturedNativeSchedules(UWorld* World)
	{
		auto& State = GCustomSafeZoneWorldState;
		if (!World || State.World != World)
			return;

		for (auto& Captured : State.CapturedManagedPhaseArrays)
		{
			if (!Captured.bStableProcessArray)
			{
				auto Owner = Captured.Owner.Get();
				if (!IsLiveCurrentIndicator(World, Owner) ||
					!Owner->HasSafeZonePhases() ||
					&Owner->SafeZonePhases != Captured.Array)
				{
					continue;
				}
			}
			if (!Captured.Array ||
				!SDK::MemReadable(
					Captured.Array,
					sizeof(TArray<FFortSafeZonePhaseInfo>)))
			{
				continue;
			}
			auto& Phases = *Captured.Array;
			if (Phases.GetData() != Captured.Data ||
				Phases.Num() != (int32)Captured.Phases.size() ||
				!ValidatePhaseArray(Phases))
			{
				continue;
			}

			for (int32 Index = 0; Index < Phases.Num(); ++Index)
			{
				auto& Phase = Phases.Get(
					Index, FFortSafeZonePhaseInfo::Size());
				const auto& Entry = Captured.Phases[Index];
				Phase.Center.X = Entry.CenterX;
				Phase.Center.Y = Entry.CenterY;
				Phase.Center.Z = Entry.CenterZ;
				Phase.Radius = float(Entry.Radius);
				if (Captured.bHasWaitTime)
					Phase.WaitTime = float(Entry.WaitTime);
				if (Captured.bHasShrinkTime)
					Phase.ShrinkTime = float(Entry.ShrinkTime);
			}
			SDK::DbgLog(
				"[CustomSafeZonePlan] restored native managed schedule phases=%d\n",
				Phases.Num());
		}

		if (State.bLegacyLocationBaselineCaptured &&
			State.GameMode &&
			State.GameMode == World->AuthorityGameMode &&
			State.GameMode->HasSafeZoneLocations())
		{
			auto& Locations = State.GameMode->SafeZoneLocations;
			const bool bCanReplace =
				Locations.Num() == 0 || Locations.IsValid();
			if (bCanReplace)
			{
				if (Locations.IsValid())
					Locations.Free();
				for (const auto& Entry :
					State.LegacyLocationBaseline)
				{
					FVector Center(
						Entry.X, Entry.Y, Entry.Z);
					Locations.Add(Center, FVector::Size());
				}
				if (State.bLegacyHadInitializedProperty &&
					State.GameMode->
						HasbSafeZoneLocationsInitialized())
				{
					State.GameMode->bSafeZoneLocationsInitialized =
						State.bLegacyWasInitialized;
				}
				SDK::DbgLog(
					"[CustomSafeZonePlan] restored legacy center schedule locations=%d\n",
					Locations.Num());
			}
		}
	}

	bool HasAuthoredHoldTiming(const FCompiledCustomSafeZonePlan& Plan)
	{
		return Plan.Nodes.size() > 1 && std::any_of(
			Plan.Nodes.begin(), Plan.Nodes.end() - 1,
			[](const FCompiledSafeZoneNode& Node)
			{
				return Node.HoldBeforeNextSeconds.has_value();
			});
	}

	bool HasAuthoredMoveTiming(const FCompiledCustomSafeZonePlan& Plan)
	{
		return Plan.Nodes.size() > 1 && std::any_of(
			Plan.Nodes.begin(), Plan.Nodes.end() - 1,
			[](const FCompiledSafeZoneNode& Node)
			{
				return Node.MoveToNextSeconds.has_value();
			});
	}

	bool ValidatePhaseTimingFields(
		const FCompiledCustomSafeZonePlan& Plan)
	{
		return (!HasAuthoredHoldTiming(Plan) ||
				FFortSafeZonePhaseInfo::HasWaitTime()) &&
			(!HasAuthoredMoveTiming(Plan) ||
				FFortSafeZonePhaseInfo::HasShrinkTime());
	}

	bool ValidateNativeTimingFields(
		AFortSafeZoneIndicator* Indicator,
		const FCompiledCustomSafeZonePlan& Plan)
	{
		if (!HasAuthoredHoldTiming(Plan) &&
			!HasAuthoredMoveTiming(Plan))
		{
			return true;
		}

		return Indicator &&
			Indicator->HasSafeZoneStartShrinkTime() &&
			Indicator->HasSafeZoneFinishShrinkTime();
	}

	bool ValidateIndicatorForMovingPhase(
		AFortSafeZoneIndicator* Indicator)
	{
		if (!Indicator ||
			!Indicator->HasNextCenter() ||
			!Indicator->HasNextRadius())
		{
			return false;
		}

		const bool bHasSourcePair =
			(Indicator->HasPreviousCenter() &&
				Indicator->HasPreviousRadius()) ||
			(Indicator->HasLastCenter() &&
				Indicator->HasLastRadius());
		return bHasSourcePair;
	}

	void MarkIndicatorDirty(
		AFortSafeZoneIndicator* Indicator,
		const wchar_t* PropertyName)
	{
		if (Indicator && PropertyName)
		{
			VersionFeatureAdapter::MarkReplicatedPropertyDirty(
				Indicator, PropertyName);
		}
	}

	void PublishCompiledGeometry(
		AFortSafeZoneIndicator* Indicator,
		const FCompiledCustomSafeZonePlan& Plan,
		int32 ActivePhase)
	{
		const int32 NodeCount = (int32)Plan.Nodes.size();
		const int32 TargetIndex = GeometryNodeIndexForPhase(
			Plan.SourcePhase, NodeCount, ActivePhase);
		if (TargetIndex < 0)
			return;

		const int32 SourceIndex = GeometryNodeIndexForPhase(
			Plan.SourcePhase, NodeCount, ActivePhase - 1);
		const int32 FutureIndex = GeometryNodeIndexForPhase(
			Plan.SourcePhase, NodeCount, ActivePhase + 1);
		const auto& Target = Plan.Nodes[TargetIndex];
		const int32 LiveIndex = SourceIndex >= 0
			? SourceIndex
			: TargetIndex;
		const auto& Live = Plan.Nodes[LiveIndex];

		// Seed the authoritative live wall at each phase boundary. The native
		// setter updates physical/material state where it exists; Radius is an
		// independent replicated live field on later indicators.
		FVector MutableLiveCenter(Live.Center);
		const bool bUsedNativePhysicalSetter =
			Indicator->TrySetSafeZoneRadiusAndCenter(
				Live.Radius, MutableLiveCenter);
		if (!bUsedNativePhysicalSetter)
		{
			// The 1.x indicator has no native setter. Its actor transform plus
			// reflected source/target radii is the same bounded physical-wall
			// fallback already used by the common pause/resume path.
			Indicator->K2_SetActorLocation(
				MutableLiveCenter, false, nullptr, true);
		}
		if (Indicator->HasRadius())
		{
			Indicator->Radius = float(Live.Radius);
			MarkIndicatorDirty(Indicator, L"Radius");
		}

		if (Indicator->HasLastCenter())
		{
			Indicator->LastCenter = FVector(Live.Center);
			MarkIndicatorDirty(Indicator, L"LastCenter");
		}
		if (Indicator->HasPreviousCenter())
		{
			Indicator->PreviousCenter = FVector(Live.Center);
			MarkIndicatorDirty(Indicator, L"PreviousCenter");
		}
		if (Indicator->HasLastRadius())
		{
			Indicator->LastRadius = float(Live.Radius);
			MarkIndicatorDirty(Indicator, L"LastRadius");
		}
		if (Indicator->HasPreviousRadius())
		{
			Indicator->PreviousRadius = float(Live.Radius);
			MarkIndicatorDirty(Indicator, L"PreviousRadius");
		}

		Indicator->NextCenter = FVector(Target.Center);
		Indicator->NextRadius = float(Target.Radius);
		MarkIndicatorDirty(Indicator, L"NextCenter");
		MarkIndicatorDirty(Indicator, L"NextRadius");

		if (FutureIndex >= 0)
		{
			const auto& Future = Plan.Nodes[FutureIndex];
			if (Indicator->HasNextNextCenter())
			{
				Indicator->NextNextCenter = FVector(Future.Center);
				MarkIndicatorDirty(Indicator, L"NextNextCenter");
			}
			if (Indicator->HasNextNextRadius())
			{
				Indicator->NextNextRadius = float(Future.Radius);
				MarkIndicatorDirty(Indicator, L"NextNextRadius");
			}
			if (Indicator->HasFutureReplicator() &&
				Indicator->FutureReplicator)
			{
				bool bFutureGeometryWritten = false;
				if (Indicator->FutureReplicator->HasNextNextCenter())
				{
					Indicator->FutureReplicator->NextNextCenter =
						FVector(Future.Center);
					bFutureGeometryWritten = true;
					VersionFeatureAdapter::MarkReplicatedPropertyDirty(
						Indicator->FutureReplicator,
						L"NextNextCenter");
				}
				if (Indicator->FutureReplicator->HasNextNextRadius())
				{
					Indicator->FutureReplicator->NextNextRadius =
						float(Future.Radius);
					bFutureGeometryWritten = true;
					VersionFeatureAdapter::MarkReplicatedPropertyDirty(
						Indicator->FutureReplicator,
						L"NextNextRadius");
				}
				// FutureReplicator is an independent actor. Dirty marking is
				// unavailable on some supported builds and cannot wake a dormant
				// actor by itself, so schedule its one boundary update explicitly.
				if (bFutureGeometryWritten)
					Indicator->FutureReplicator->ForceNetUpdate();
			}
		}
	}

	void ApplyNativeTimingOverride(
		AFortSafeZoneIndicator* Indicator,
		const FCompiledCustomSafeZonePlan& Plan,
		int32 ActivePhase,
		float TimeSeconds)
	{
		const int32 TimingIndex =
			TimingSourceNodeIndexForPhase(
				Plan.RuntimeStartPhase,
				(int32)Plan.Nodes.size(),
				ActivePhase);
		if (TimingIndex < 0)
			return;

		const auto& Source = Plan.Nodes[TimingIndex];
		if (!Source.HoldBeforeNextSeconds.has_value() &&
			!Source.MoveToNextSeconds.has_value())
		{
			return;
		}
		if (!Indicator->HasSafeZoneStartShrinkTime() ||
			!Indicator->HasSafeZoneFinishShrinkTime() ||
			!std::isfinite(TimeSeconds))
		{
			return;
		}

		const float NativeWait = (std::max)(
			0.f, Indicator->SafeZoneStartShrinkTime - TimeSeconds);
		const float NativeMove = (std::max)(
			0.f,
			Indicator->SafeZoneFinishShrinkTime -
				Indicator->SafeZoneStartShrinkTime);
		const float Hold = ResolveAuthoredDuration(
			Source.HoldBeforeNextSeconds, NativeWait);
		const float Move = ResolveAuthoredDuration(
			Source.MoveToNextSeconds, NativeMove);
		Indicator->SafeZoneStartShrinkTime = TimeSeconds + Hold;
		Indicator->SafeZoneFinishShrinkTime =
			Indicator->SafeZoneStartShrinkTime + Move;
		MarkIndicatorDirty(
			Indicator, L"SafeZoneStartShrinkTime");
		MarkIndicatorDirty(
			Indicator, L"SafeZoneFinishShrinkTime");
	}
}

void CustomSafeZoneRuntime::ResetForMatch(UWorld* World)
{
	// Custom geometry/timing is an overlay on Fortnite's native schedule. A
	// reused owner must begin the next match from the exact values captured
	// before the prior overlay, especially when new timings inherit native data.
	RestoreCapturedNativeSchedules(World);
	const auto RestartSnapshot =
		FConfiguration::GetCustomSafeZoneSequenceSnapshot();
	const bool bNeedsRestartPreflight =
		IsMovingConfigurationEnabled(RestartSnapshot) &&
		HasMovingTransition(RestartSnapshot);
	FConfiguration::ReleaseCustomSafeZoneSequenceForMatch();
	GCustomSafeZoneWorldState = {};
	GCustomSafeZoneWorldState.World = World;
	GCustomSafeZoneWorldState.GameMode = World
		? (AFortGameMode*)World->AuthorityGameMode
		: nullptr;
	GCustomSafeZoneWorldState.GameState = World
		? (AFortGameStateAthena*)World->GameState
		: nullptr;
	GCustomSafeZoneWorldState.Status.World = World;
	GCustomSafeZoneWorldState.Status.Failure =
		ECustomSafeZoneRuntimeFailure::NotRequested;
	GCustomSafeZoneWorldState.bRequiresPreflightBeforeApply =
		bNeedsRestartPreflight;
	PublishVisibleStatus(GCustomSafeZoneWorldState.Status);
	SetMatchRestartPreflightPending(
		World, bNeedsRestartPreflight);

	// The lifecycle owner immediately performs a full preflight for a same-world
	// restart.  Do not freeze here: accepting an unvalidated draft would bypass
	// the reflected owner, timing, capacity, projection, and radius-order checks.
	SDK::DbgLog(
		"[CustomSafeZonePlan] match state reset world=%p\n",
		(void*)World);
}

bool CustomSafeZoneRuntime::IsMatchRestartPreflightPending(
	UWorld* World)
{
	if (!World || !GMatchRestartPreflightPending.load(
			std::memory_order_acquire))
	{
		return false;
	}

	const bool bSameGeneration =
		GMatchRestartPreflightWorld.load(
			std::memory_order_relaxed) == World &&
		GMatchRestartPreflightGameMode.load(
			std::memory_order_relaxed) ==
			(AFortGameMode*)World->AuthorityGameMode &&
		GMatchRestartPreflightGameState.load(
			std::memory_order_relaxed) ==
			(AFortGameStateAthena*)World->GameState;
	if (!bSameGeneration)
	{
		// Travel or owner replacement supersedes the rejected generation. Do
		// not let its process-static latch hold the new readiness hook.
		SetMatchRestartPreflightPending(nullptr, false);
	}
	return bSameGeneration;
}

void CustomSafeZoneRuntime::SetAuthoritativePhasePublisherAvailable(
	bool bAvailable)
{
	GAuthoritativePhasePublisherCapability.store(
		bAvailable ? 1 : 0, std::memory_order_release);
}

bool CustomSafeZoneRuntime::
	IsAuthoritativePhasePublisherCapabilityKnown()
{
	return GAuthoritativePhasePublisherCapability.load(
		std::memory_order_acquire) >= 0;
}

bool CustomSafeZoneRuntime::IsAuthoritativePhasePublisherAvailable()
{
	return GAuthoritativePhasePublisherCapability.load(
		std::memory_order_acquire) > 0;
}

void CustomSafeZoneRuntime::SetLegacyPhaseOwnerPath(
	ECustomSafeZoneLegacyPhaseOwnerPath OwnerPath)
{
	GLegacyPhaseOwnerPath.store(
		OwnerPath, std::memory_order_release);
}

ECustomSafeZoneLegacyPhaseOwnerPath
	CustomSafeZoneRuntime::GetLegacyPhaseOwnerPath()
{
	return GLegacyPhaseOwnerPath.load(
		std::memory_order_acquire);
}

UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
	CustomSafeZoneRuntime::ResolveLiveComponentPhaseLogic(
		UWorld* World)
{
	return ResolveLiveComponentPhaseLogicInternal(World);
}

bool CustomSafeZoneRuntime::IsMovingModeRequested()
{
	auto World = UWorld::GetWorld();
	if (GCustomSafeZoneWorldState.bPlanCommitted &&
		GCustomSafeZoneWorldState.Plan &&
		GCustomSafeZoneWorldState.World == World)
	{
		return true;
	}
	if (GCustomSafeZoneWorldState.bPreflightAccepted &&
		GCustomSafeZoneWorldState.Snapshot &&
		GCustomSafeZoneWorldState.World == World)
	{
		return true;
	}
	auto Snapshot =
		FConfiguration::GetCustomSafeZoneSequenceSnapshot();
	if (!IsMovingConfigurationEnabled(Snapshot))
		return false;
	// A one-circle snapshot normally remains the legacy stationary
	// representation. Opting into final closure adds an implicit radius-zero
	// target, making that same draft a real one-transition moving plan.
	if (!HasMovingTransition(Snapshot))
		return false;

	if (!IsSupportedMovingZoneVersion())
	{
		static bool bLoggedUnsupportedVersion = false;
		if (!bLoggedUnsupportedVersion)
		{
			bLoggedUnsupportedVersion = true;
			SDK::DbgLog(
				"[CustomSafeZonePlan] status=ignored code=%s "
				"version=%.2f; stationary custom zone retained\n",
				GetFailureName(
					ECustomSafeZoneRuntimeFailure::UnsupportedVersion),
				VersionInfo.FortniteVersion);
		}
		return false;
	}
	return true;
}

int32 CustomSafeZoneRuntime::ResolveRuntimeStartPhase()
{
	const int32 ConfiguredStart =
		FConfiguration::LateGameZone.load(std::memory_order_acquire);
	return LogicalToRuntimeStartPhase(
		ConfiguredStart, VersionInfo.FortniteVersion);
}

int32 CustomSafeZoneRuntime::ResolveNativePhaseCapacity(
	AFortAthenaMapInfo* MapInfo)
{
	return ResolvePhaseCapacity(MapInfo, 0);
}

bool CustomSafeZoneRuntime::DoesSequenceFitPhaseCapacity(
	int32 NodeCount,
	int32 PhaseCapacity,
	bool bCloseFinalCircle)
{
	return HasCapacityForPlan(
		ResolveRuntimeStartPhase(),
		EffectiveNodeCount(NodeCount, bCloseFinalCircle),
		PhaseCapacity);
}

ECustomSafeZonePreflightResult
	CustomSafeZoneRuntime::PreflightForServerStart(
		UWorld* World,
		AFortGameMode* GameMode,
		AFortAthenaMapInfo* MapInfo,
		int32 PhaseCapacity,
		bool bTreatUnavailableAsInvalid,
		bool bCanCorrectBeforeListen)
{
	ResetStateForWorld(World);
	auto& State = GCustomSafeZoneWorldState;
	if (State.bPreflightAccepted && State.Snapshot &&
		State.MapInfo == MapInfo && State.GameMode == GameMode)
	{
		return ECustomSafeZonePreflightResult::Ready;
	}
	auto Snapshot =
		FConfiguration::GetCustomSafeZoneSequenceSnapshot();
	const int32 RuntimeStartPhase = ResolveRuntimeStartPhase();
	ResetStateForCacheKey(
		MapInfo, Snapshot, RuntimeStartPhase, PhaseCapacity);

	if (!IsMovingConfigurationEnabled(Snapshot) ||
		!HasMovingTransition(Snapshot))
	{
		State.bRequiresPreflightBeforeApply = false;
		SetMatchRestartPreflightPending(nullptr, false);
		State.Status.Failure =
			ECustomSafeZoneRuntimeFailure::NotRequested;
		State.Status.RuntimeStartPhase = RuntimeStartPhase;
		State.Status.NodeCount =
			Snapshot ? (int32)Snapshot->Nodes.size() : 0;
		State.Status.PhaseCapacity = PhaseCapacity;
		State.Status.bCloseFinalCircle =
			Snapshot && Snapshot->bCloseFinalCircle;
		State.Status.OffendingEdge = -1;
		State.Status.bCanCorrectBeforeListen = false;
		PublishVisibleStatus(State.Status);
		return ECustomSafeZonePreflightResult::Ready;
	}

	const int32 NodeCount = (int32)Snapshot->Nodes.size();
	const int32 CompiledNodeCount = EffectiveNodeCount(*Snapshot);
	auto PublishPending = [&]()
	{
		State.Status.Failure =
			ECustomSafeZoneRuntimeFailure::NotRequested;
		State.Status.RuntimeStartPhase = RuntimeStartPhase;
		State.Status.NodeCount = NodeCount;
		State.Status.PhaseCapacity = PhaseCapacity;
		State.Status.bCloseFinalCircle =
			Snapshot->bCloseFinalCircle;
		State.Status.OffendingEdge = -1;
		State.Status.bCanCorrectBeforeListen = false;
		PublishVisibleStatus(State.Status);
	};
	const auto RadiusIncreaseEdge =
		Snapshot->FindFirstRadiusIncreaseEdge();
	if (RadiusIncreaseEdge.has_value())
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::RadiusIncreaseNotAllowed,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"a circle is larger than its preceding circle",
			(int32)*RadiusIncreaseEdge);
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	if (!World || !MapInfo)
	{
		if (bTreatUnavailableAsInvalid)
		{
			SetFailure(
				World
					? ECustomSafeZoneRuntimeFailure::MissingMapInfo
					: ECustomSafeZoneRuntimeFailure::MissingWorld,
				RuntimeStartPhase, NodeCount, 0,
				World
					? "live Athena map info did not appear before timeout"
					: "authoritative world did not appear before timeout");
			MarkPreflightRejected(bCanCorrectBeforeListen);
			return ECustomSafeZonePreflightResult::Invalid;
		}
		PublishPending();
		return ECustomSafeZonePreflightResult::Pending;
	}

	if (!IsAuthoritativePhasePublisherCapabilityKnown())
	{
		if (!bTreatUnavailableAsInvalid)
		{
			PublishPending();
			return ECustomSafeZonePreflightResult::Pending;
		}
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhasePublisher,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"authoritative phase hook discovery did not complete before timeout");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	if (!IsAuthoritativePhasePublisherAvailable())
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhasePublisher,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"authoritative phase publisher failed post-hook verification");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	if (PhaseCapacity <= 0)
	{
		if (!bTreatUnavailableAsInvalid)
		{
			PublishPending();
			return ECustomSafeZonePreflightResult::Pending;
		}
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhaseFields,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"live map does not expose a finite integral phase capacity");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}
	if (PhaseCapacity > 64)
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhaseFields,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"live phase capacity exceeds the guarded native limit");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	if (!HasCapacityForPlan(
			RuntimeStartPhase, CompiledNodeCount, PhaseCapacity))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::InsufficientPhaseCapacity,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"authored nodes exceed the live map/playlist phase schedule");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	const bool bNeedsMapProjection = std::any_of(
		Snapshot->Nodes.begin(), Snapshot->Nodes.end(),
		[](const FCustomSafeZoneNode& Node)
		{
			return Node.bHasNormalizedCenter;
		});
	if (bNeedsMapProjection &&
		!GUI::IsCustomSafeZoneMapProjectionReady(MapInfo))
	{
		if (!bTreatUnavailableAsInvalid)
		{
			PublishPending();
			return ECustomSafeZonePreflightResult::Pending;
		}
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MapProjectionUnavailable,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"authoritative map projection did not appear before timeout");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	AFortSafeZoneIndicator* IndicatorPreflight = nullptr;
	bool bOwnerReady = GameMode && GameMode->Class;
	if (VersionInfo.FortniteVersion >= 25.20f)
	{
		auto PhaseLogic = ResolveLiveComponentPhaseLogic(World);
		bOwnerReady = PhaseLogic && PhaseLogic->Class &&
			!PhaseLogic->IsDefaultObject();
		if (bOwnerReady && PhaseLogic->HasSafeZoneIndicator() &&
			PhaseLogic->SafeZoneIndicator &&
			PhaseLogic->SafeZoneIndicator->Class)
		{
			IndicatorPreflight = PhaseLogic->SafeZoneIndicator;
		}
		if (!IndicatorPreflight && bOwnerReady &&
			PhaseLogic->HasSafeZoneIndicatorClass())
		{
			const UClass* IndicatorClass =
				PhaseLogic->SafeZoneIndicatorClass.Get();
			if (IndicatorClass && IndicatorClass->Class)
			{
				IndicatorPreflight = (AFortSafeZoneIndicator*)
					IndicatorClass->GetDefaultObj();
			}
		}
	}
	else if (bOwnerReady)
	{
		if (GameMode->HasSafeZoneIndicator() &&
			GameMode->SafeZoneIndicator &&
			GameMode->SafeZoneIndicator->Class)
		{
			IndicatorPreflight = GameMode->SafeZoneIndicator;
		}
		if (!IndicatorPreflight &&
			GameMode->HasSafeZoneIndicatorClass())
		{
			const UClass* IndicatorClass =
				GameMode->SafeZoneIndicatorClass.Get();
			if (IndicatorClass && IndicatorClass->Class)
			{
				IndicatorPreflight = (AFortSafeZoneIndicator*)
					IndicatorClass->GetDefaultObj();
			}
		}
	}

	if (!bOwnerReady || !IndicatorPreflight ||
		!IndicatorPreflight->Class)
	{
		if (!bTreatUnavailableAsInvalid)
		{
			PublishPending();
			return ECustomSafeZonePreflightResult::Pending;
		}
		SetFailure(
			bOwnerReady
				? ECustomSafeZoneRuntimeFailure::MissingIndicatorFields
				: ECustomSafeZoneRuntimeFailure::MissingPhasePublisher,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			bOwnerReady
				? "safe-zone indicator class did not appear before timeout"
				: "authoritative phase owner did not appear before timeout");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	// Compile against the live map before networking starts. This validates
	// projection, geometry, timing, radius ordering, and the reflected phase
	// publisher while the host can still correct the draft. The resulting plan
	// retains this exact immutable snapshot for the match.
	State.bPreflightValidationInProgress = true;
	const auto ValidatedPlan =
		GetOrCompilePlan(World, MapInfo, PhaseCapacity);
	State.bPreflightValidationInProgress = false;
	auto CurrentSnapshot =
		FConfiguration::GetCustomSafeZoneSequenceSnapshot();
	if (CurrentSnapshot != Snapshot ||
		ResolveRuntimeStartPhase() != RuntimeStartPhase ||
		!IsMovingConfigurationEnabled(CurrentSnapshot))
	{
		ResetStateForCacheKey(
			MapInfo, CurrentSnapshot,
			ResolveRuntimeStartPhase(), PhaseCapacity);
		return ECustomSafeZonePreflightResult::Pending;
	}
	if (!ValidatedPlan)
	{
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	if (!ValidateIndicatorForMovingPhase(IndicatorPreflight))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingIndicatorFields,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"indicator lacks reflected source/target center or radius fields");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}

	if (VersionInfo.FortniteVersion >= 21.10f)
	{
		// FN21.10-25.19 legitimately stores the managed schedule in the
		// GameMode-owned fallback array when the indicator lacks this property.
		// Component-owned builds have no such fallback and require it directly.
		const bool bRequiresIndicatorOwnedPhases =
			VersionInfo.FortniteVersion >= 25.20f;
		if ((bRequiresIndicatorOwnedPhases &&
				!IndicatorPreflight->HasSafeZonePhases()) ||
			!FFortSafeZonePhaseInfo::HasCenter() ||
			!FFortSafeZonePhaseInfo::HasRadius() ||
			!ValidatePhaseTimingFields(*ValidatedPlan))
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::MissingPhaseFields,
				RuntimeStartPhase, NodeCount, PhaseCapacity,
				"managed phase array lacks reflected geometry or timing fields");
			MarkPreflightRejected(bCanCorrectBeforeListen);
			return ECustomSafeZonePreflightResult::Invalid;
		}
	}
	else if (!ValidateNativeTimingFields(
			IndicatorPreflight, *ValidatedPlan))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingIndicatorFields,
			RuntimeStartPhase, NodeCount, PhaseCapacity,
			"native indicator lacks reflected authored-timing deadlines");
		MarkPreflightRejected(bCanCorrectBeforeListen);
		return ECustomSafeZonePreflightResult::Invalid;
	}
	CurrentSnapshot =
		FConfiguration::GetCustomSafeZoneSequenceSnapshot();
	if (CurrentSnapshot != Snapshot ||
		ResolveRuntimeStartPhase() != RuntimeStartPhase ||
		!IsMovingConfigurationEnabled(CurrentSnapshot) ||
		!FConfiguration::FreezeCustomSafeZoneSequenceForMatch(
			Snapshot))
	{
		ResetStateForCacheKey(
			MapInfo, CurrentSnapshot,
			ResolveRuntimeStartPhase(), PhaseCapacity);
		return ECustomSafeZonePreflightResult::Pending;
	}

	State.bActivationFailed = false;
	State.bPreflightAccepted = true;
	State.bRequiresPreflightBeforeApply = false;
	SetMatchRestartPreflightPending(nullptr, false);
	SetReadyStatus(*ValidatedPlan);
	return ECustomSafeZonePreflightResult::Ready;
}

bool CustomSafeZoneRuntime::ApplyToPhaseArray(
	UWorld* World,
	AFortAthenaMapInfo* MapInfo,
	AFortSafeZoneIndicator* Indicator,
	TArray<FFortSafeZonePhaseInfo>& Phases,
	const char* Source,
	bool bStableProcessArray)
{
	if (!IsMovingModeRequested())
		return false;
	ResetStateForWorld(World);
	if (!ValidatePhaseArray(Phases))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhaseFields,
			ResolveRuntimeStartPhase(), 0, Phases.Num(),
			"phase array schema is unavailable or corrupt");
		return false;
	}

	auto Plan = GetOrCompilePlan(World, MapInfo, Phases.Num());
	if (!Plan)
		return false;
	if (!HasCapacityForPlan(
			Plan->RuntimeStartPhase,
			(int32)Plan->Nodes.size(), Phases.Num()) ||
		!ValidatePhaseTimingFields(*Plan))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhaseFields,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, Phases.Num(),
			"phase array lacks capacity or authored timing fields");
		return false;
	}
	if (!ValidateIndicatorForMovingPhase(Indicator))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingIndicatorFields,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, Phases.Num(),
			"indicator lacks source/target geometry fields");
		return false;
	}
	if (!CaptureManagedPhaseArray(
			Indicator, Phases, bStableProcessArray))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhaseFields,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, Phases.Num(),
			"native phase schedule could not be captured atomically");
		return false;
	}

	const int32 PhaseCount = Phases.Num();
	const int32 PhaseSize = FFortSafeZonePhaseInfo::Size();
	// This is intentionally a narrow overlay: damage, player caps, grid data,
	// and every other native phase field remain authored by Fortnite. That is
	// especially important after a radius-zero close, where later native phases
	// still advance damage while repeating the closed geometry.
	for (int32 Phase = Plan->SourcePhase;
		Phase < PhaseCount; ++Phase)
	{
		const int32 GeometryIndex = GeometryNodeIndexForPhase(
			Plan->SourcePhase,
			(int32)Plan->Nodes.size(), Phase);
		auto& PhaseInfo = Phases.Get(Phase, PhaseSize);
		const auto& Node = Plan->Nodes[GeometryIndex];
		PhaseInfo.Center = FVector(Node.Center);
		PhaseInfo.Radius = float(Node.Radius);

		const int32 TimingIndex =
			TimingSourceNodeIndexForPhase(
				Plan->RuntimeStartPhase,
				(int32)Plan->Nodes.size(), Phase);
		if (TimingIndex < 0)
			continue;

		const auto& TimingNode = Plan->Nodes[TimingIndex];
		if (TimingNode.HoldBeforeNextSeconds.has_value())
			PhaseInfo.WaitTime =
				float(*TimingNode.HoldBeforeNextSeconds);
		if (TimingNode.MoveToNextSeconds.has_value())
			PhaseInfo.ShrinkTime =
				float(*TimingNode.MoveToNextSeconds);
	}
	MarkPlanActivated(*Plan);

	auto& State = GCustomSafeZoneWorldState;
	if (!State.bPhaseArrayLogged)
	{
		State.bPhaseArrayLogged = true;
		SDK::DbgLog(
			"[CustomSafeZonePlan] applied phase-array source=%s "
			"start=%d sourcePhase=%d nodes=%d phases=%d\n",
			Source ? Source : "unknown",
			Plan->RuntimeStartPhase, Plan->SourcePhase,
			(int32)Plan->Nodes.size(), PhaseCount);
	}
	return true;
}

bool CustomSafeZoneRuntime::ApplyToLegacyLocations(
	AFortGameMode* GameMode,
	AFortAthenaMapInfo* MapInfo,
	int32 RequiredLocationCount,
	bool bHasNativePhasePublisher)
{
	if (!IsMovingModeRequested())
		return false;
	auto World = UWorld::GetWorld();
	ResetStateForWorld(World);
	if (!bHasNativePhasePublisher)
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhasePublisher,
			ResolveRuntimeStartPhase(), 0,
			RequiredLocationCount,
			"native moving-zone phase publisher is unavailable");
		return false;
	}
	if (!GameMode || !GameMode->HasSafeZoneLocations() ||
		RequiredLocationCount <= 0 ||
		RequiredLocationCount > 64 ||
		FVector::Size() <= 0 || FVector::Size() > 0x40)
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::InvalidLegacyLocations,
			ResolveRuntimeStartPhase(), 0,
			RequiredLocationCount,
			"legacy location array is unavailable");
		return false;
	}

	auto& Locations = GameMode->SafeZoneLocations;
	const int32 ExistingCount = Locations.Num();
	const int32 OutputCount = (std::max)(
		ExistingCount, RequiredLocationCount);
	if (ExistingCount < 0 || OutputCount > 64 ||
		Locations.Max() < ExistingCount ||
		Locations.Max() > 128 ||
		(ExistingCount > 0 &&
			!SDK::MemReadable(
				Locations.GetData(),
				(size_t)ExistingCount * FVector::Size())))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::InvalidLegacyLocations,
			ResolveRuntimeStartPhase(), 0, OutputCount,
			"legacy location array failed structural validation");
		return false;
	}

	auto Plan = GetOrCompilePlan(World, MapInfo, OutputCount);
	if (!Plan)
		return false;

	// When the version has already published its indicator class/instance, use
	// that CDO as an early reflection preflight. Some very old builds create the
	// class later; those are still protected by the rollback in SetFailure.
	AFortSafeZoneIndicator* IndicatorPreflight =
		GameMode->HasSafeZoneIndicator()
			? GameMode->SafeZoneIndicator
			: nullptr;
	if (!IndicatorPreflight && GameMode->HasSafeZoneIndicatorClass())
	{
		const UClass* IndicatorClass =
			GameMode->SafeZoneIndicatorClass.Get();
		if (IndicatorClass)
		{
			IndicatorPreflight = (AFortSafeZoneIndicator*)
				IndicatorClass->GetDefaultObj();
		}
	}
	if (IndicatorPreflight &&
		(!ValidateIndicatorForMovingPhase(IndicatorPreflight) ||
		 !ValidateNativeTimingFields(IndicatorPreflight, *Plan)))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingIndicatorFields,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, OutputCount,
			"legacy indicator CDO lacks moving-zone fields");
		return false;
	}

	if (!GCustomSafeZoneWorldState.bLegacyLocationBaselineCaptured)
	{
		auto& CaptureState = GCustomSafeZoneWorldState;
		CaptureState.LegacyLocationBaseline.reserve(
			(size_t)ExistingCount);
		for (int32 Index = 0; Index < ExistingCount; ++Index)
		{
			const auto& Center = Locations.Get(
				Index, FVector::Size());
			CaptureState.LegacyLocationBaseline.push_back({
				Center.X, Center.Y, Center.Z
			});
		}
		CaptureState.bLegacyHadInitializedProperty =
			GameMode->HasbSafeZoneLocationsInitialized();
		CaptureState.bLegacyWasInitialized =
			CaptureState.bLegacyHadInitializedProperty
				? (bool)GameMode->bSafeZoneLocationsInitialized
				: false;
		CaptureState.bLegacyLocationBaselineCaptured = true;
	}

	// Stage the entire result first. Validation failure can never leave only a
	// prefix of the native location source authored.
	std::vector<FVector> StagedLocations((size_t)OutputCount);
	for (int32 Index = 0; Index < OutputCount; ++Index)
		StagedLocations[Index] =
			FVector(Plan->Nodes.front().Center);
	for (int32 Index = 0; Index < ExistingCount; ++Index)
		StagedLocations[Index] =
			Locations.Get(Index, FVector::Size());
	for (int32 Phase = Plan->SourcePhase;
		Phase < OutputCount; ++Phase)
	{
		const int32 NodeIndex = GeometryNodeIndexForPhase(
			Plan->SourcePhase,
			(int32)Plan->Nodes.size(), Phase);
		StagedLocations[Phase] =
			FVector(Plan->Nodes[NodeIndex].Center);
	}

	for (int32 Index = 0; Index < ExistingCount; ++Index)
		Locations.Get(Index, FVector::Size()) = StagedLocations[Index];
	for (int32 Index = ExistingCount;
		Index < OutputCount; ++Index)
	{
		Locations.Add(StagedLocations[Index], FVector::Size());
	}
	if (GameMode->HasbSafeZoneLocationsInitialized())
		GameMode->bSafeZoneLocationsInitialized = true;

	auto& State = GCustomSafeZoneWorldState;
	State.bPlanCommitted = true;
	State.bLegacyLocationsCommitted = true;
	State.LegacyCommittedStartPhase = Plan->SourcePhase;
	State.LegacyCommittedLocationCount = Locations.Num();
	SetReadyStatus(*Plan);
	if (!State.bLegacyLocationsLogged)
	{
		State.bLegacyLocationsLogged = true;
		SDK::DbgLog(
			"[CustomSafeZonePlan] applied legacy locations "
			"start=%d sourcePhase=%d nodes=%d locations=%d->%d\n",
			Plan->RuntimeStartPhase, Plan->SourcePhase,
			(int32)Plan->Nodes.size(), ExistingCount,
			Locations.Num());
	}
	return true;
}

bool CustomSafeZoneRuntime::ApplyNativePhase(
	AFortGameMode* GameMode,
	AFortAthenaMapInfo* MapInfo,
	int32 ActivePhase,
	float TimeSeconds,
	const char* Source,
	bool bHasNativePhasePublisher)
{
	if (!IsMovingModeRequested())
		return false;
	auto World = UWorld::GetWorld();
	ResetStateForWorld(World);
	auto Indicator = GameMode && GameMode->HasSafeZoneIndicator()
		? GameMode->SafeZoneIndicator
		: nullptr;
	int32 PhaseCapacity = 0;
	if (Indicator && Indicator->HasSafeZonePhases() &&
		Indicator->SafeZonePhases.IsValid())
	{
		PhaseCapacity = Indicator->SafeZonePhases.Num();
	}
	if (PhaseCapacity <= 0 && GameMode &&
		GameMode->HasSafeZoneLocations())
	{
		PhaseCapacity = GameMode->SafeZoneLocations.Num();
	}
	PhaseCapacity = ResolvePhaseCapacity(MapInfo, PhaseCapacity);
	auto Plan = GetOrCompilePlan(World, MapInfo, PhaseCapacity);
	if (!Plan)
		return false;
	if (!bHasNativePhasePublisher)
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhasePublisher,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, PhaseCapacity,
			"native moving-zone phase publisher is unavailable");
		return false;
	}
	if (!ValidateIndicatorForMovingPhase(Indicator))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingIndicatorFields,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, PhaseCapacity,
			"native indicator lacks source/target geometry fields");
		return false;
	}
	if (!ValidateNativeTimingFields(Indicator, *Plan))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingIndicatorFields,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, PhaseCapacity,
			"native indicator lacks authored timing deadlines");
		return false;
	}
	if (Indicator->HasSafeZonePhases() &&
		Indicator->SafeZonePhases.IsValid() &&
		(!ValidatePhaseArray(Indicator->SafeZonePhases) ||
			!HasCapacityForPlan(
				Plan->RuntimeStartPhase,
				(int32)Plan->Nodes.size(),
				Indicator->SafeZonePhases.Num()) ||
			!ValidatePhaseTimingFields(*Plan)))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhaseFields,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount,
			Indicator->SafeZonePhases.Num(),
			"native phase array lacks moving-zone fields");
		return false;
	}
	if (Indicator->HasSafeZonePhases() &&
		Indicator->SafeZonePhases.IsValid())
	{
		if (!ApplyToPhaseArray(
				World, MapInfo, Indicator,
				Indicator->SafeZonePhases, Source))
		{
			return false;
		}
	}
	if (ActivePhase < Plan->SourcePhase)
	{
		MarkPlanActivated(*Plan);
		return true;
	}

	PublishCompiledGeometry(Indicator, *Plan, ActivePhase);
	ApplyNativeTimingOverride(
		Indicator, *Plan, ActivePhase, TimeSeconds);
	Indicator->ForceNetUpdate();
	MarkPhysicalWallPublished(*Plan, Indicator);

	auto& State = GCustomSafeZoneWorldState;
	if (State.LastPublishedPhase != ActivePhase)
	{
		State.LastPublishedPhase = ActivePhase;
		const int32 TargetIndex = GeometryNodeIndexForPhase(
			Plan->SourcePhase,
			(int32)Plan->Nodes.size(), ActivePhase);
		const auto& Target = Plan->Nodes[TargetIndex];
		SDK::DbgLog(
			"[CustomSafeZonePlan] published native phase source=%s "
			"phase=%d node=%d center=(%.1f,%.1f,%.1f) radius=%.1f\n",
			Source ? Source : "unknown", ActivePhase,
			TargetIndex, Target.Center.X, Target.Center.Y,
			Target.Center.Z, Target.Radius);
	}
	return true;
}

bool CustomSafeZoneRuntime::PublishManagedPhase(
	UWorld* World,
	AFortAthenaMapInfo* MapInfo,
	AFortSafeZoneIndicator* Indicator,
	int32 ActivePhase,
	const char* Source,
	TArray<FFortSafeZonePhaseInfo>* ManagedPhases,
	bool bStableProcessArray)
{
	if (!IsMovingModeRequested())
		return false;
	ResetStateForWorld(World);
	auto IndicatorPhases =
		Indicator && Indicator->HasSafeZonePhases() &&
			Indicator->SafeZonePhases.IsValid()
			? &Indicator->SafeZonePhases
			: nullptr;
	auto PhaseArray = ManagedPhases && ManagedPhases->IsValid()
		? ManagedPhases
		: IndicatorPhases;
	const int32 PhaseCapacity = PhaseArray
		? PhaseArray->Num()
		: ResolvePhaseCapacity(MapInfo, 0);
	auto Plan = GetOrCompilePlan(
		World, MapInfo, PhaseCapacity);
	if (!Plan)
		return false;
	if (!HasCapacityForPlan(
			Plan->RuntimeStartPhase,
			(int32)Plan->Nodes.size(), PhaseCapacity))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::InsufficientPhaseCapacity,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, PhaseCapacity,
			"managed owner capacity changed after plan compilation");
		return false;
	}
	if (!ValidateIndicatorForMovingPhase(Indicator))
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingIndicatorFields,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, PhaseCapacity,
			"managed indicator lacks source/target geometry fields");
		return false;
	}

	auto& State = GCustomSafeZoneWorldState;
	if (!State.bActivated)
	{
		if (!PhaseArray ||
			!ApplyToPhaseArray(
				World, MapInfo, Indicator,
				*PhaseArray, Source,
				bStableProcessArray))
		{
			SetFailure(
				ECustomSafeZoneRuntimeFailure::MissingPhaseFields,
				Plan->RuntimeStartPhase,
				Plan->AuthoredNodeCount, PhaseCapacity,
				"managed phase plan was not applied during setup");
			return false;
		}
	}
	if (ActivePhase < Plan->SourcePhase)
		return true;

	PublishCompiledGeometry(Indicator, *Plan, ActivePhase);
	Indicator->ForceNetUpdate();
	MarkPhysicalWallPublished(*Plan, Indicator);

	if (State.LastPublishedPhase != ActivePhase)
	{
		State.LastPublishedPhase = ActivePhase;
		const int32 TargetIndex = GeometryNodeIndexForPhase(
			Plan->SourcePhase,
			(int32)Plan->Nodes.size(), ActivePhase);
		const auto& Target = Plan->Nodes[TargetIndex];
		SDK::DbgLog(
			"[CustomSafeZonePlan] published managed phase source=%s "
			"phase=%d node=%d center=(%.1f,%.1f,%.1f) radius=%.1f\n",
			Source ? Source : "unknown", ActivePhase,
			TargetIndex, Target.Center.X, Target.Center.Y,
			Target.Center.Z, Target.Radius);
	}
	return true;
}

bool CustomSafeZoneRuntime::TryGetSourceCircle(
	UWorld* World,
	AFortAthenaMapInfo* MapInfo,
	FVector& OutCenter,
	float& OutRadius,
	int32 PhaseCapacityHint,
	bool bHasNativePhasePublisher)
{
	OutCenter = {};
	OutRadius = 0.f;
	if (!IsMovingModeRequested())
		return false;

	auto Plan = GetOrCompilePlan(
		World, MapInfo,
		ResolvePhaseCapacity(MapInfo, PhaseCapacityHint));
	if (!Plan || Plan->Nodes.empty())
		return false;
	if (VersionInfo.FortniteVersion < 21.10f &&
		!bHasNativePhasePublisher)
	{
		SetFailure(
			ECustomSafeZoneRuntimeFailure::MissingPhasePublisher,
			Plan->RuntimeStartPhase,
			Plan->AuthoredNodeCount, Plan->PhaseCapacity,
			"native moving-zone phase publisher is unavailable");
		return false;
	}
	OutCenter = FVector(Plan->Nodes.front().Center);
	OutRadius = Plan->Nodes.front().Radius;
	return true;
}

bool CustomSafeZoneRuntime::IsMovingPlanActive(
	UWorld* World,
	AFortSafeZoneIndicator* Indicator)
{
	const auto& State = GCustomSafeZoneWorldState;
	return World && State.World == World && State.Plan &&
		State.bActivated && State.bPhysicalPublished &&
		State.PublishedIndicator &&
		(!Indicator || State.PublishedIndicator == Indicator) &&
		State.Status.Failure == ECustomSafeZoneRuntimeFailure::None;
}

FCustomSafeZoneRuntimeStatus CustomSafeZoneRuntime::GetStatus()
{
	auto& Visible = GVisibleCustomSafeZoneStatus;
	for (;;)
	{
		const uint64 Before =
			Visible.Sequence.load(std::memory_order_acquire);
		if (Before & 1)
			continue;

		FCustomSafeZoneRuntimeStatus Status;
		Status.Failure =
			Visible.Failure.load(std::memory_order_relaxed);
		Status.World = Visible.World.load(std::memory_order_relaxed);
		Status.RuntimeStartPhase = Visible.RuntimeStartPhase.load(
			std::memory_order_relaxed);
		Status.NodeCount =
			Visible.NodeCount.load(std::memory_order_relaxed);
		Status.PhaseCapacity =
			Visible.PhaseCapacity.load(std::memory_order_relaxed);
		Status.bCloseFinalCircle =
			Visible.CloseFinalCircle.load(
				std::memory_order_relaxed);
		Status.OffendingEdge =
			Visible.OffendingEdge.load(std::memory_order_relaxed);
		Status.bCanCorrectBeforeListen =
			Visible.CanCorrectBeforeListen.load(
				std::memory_order_relaxed);

		const uint64 After =
			Visible.Sequence.load(std::memory_order_acquire);
		if (Before == After)
			return Status;
	}
}

const char* CustomSafeZoneRuntime::GetFailureName(
	ECustomSafeZoneRuntimeFailure Failure)
{
	switch (Failure)
	{
	case ECustomSafeZoneRuntimeFailure::None:
		return "none";
	case ECustomSafeZoneRuntimeFailure::NotRequested:
		return "not-requested";
	case ECustomSafeZoneRuntimeFailure::UnsupportedVersion:
		return "unsupported-version";
	case ECustomSafeZoneRuntimeFailure::MissingWorld:
		return "missing-world";
	case ECustomSafeZoneRuntimeFailure::MissingMapInfo:
		return "missing-map-info";
	case ECustomSafeZoneRuntimeFailure::MissingSnapshot:
		return "missing-snapshot";
	case ECustomSafeZoneRuntimeFailure::TooFewNodes:
		return "too-few-nodes";
	case ECustomSafeZoneRuntimeFailure::TooManyNodes:
		return "too-many-nodes";
	case ECustomSafeZoneRuntimeFailure::InvalidNode:
		return "invalid-node";
	case ECustomSafeZoneRuntimeFailure::RadiusIncreaseNotAllowed:
		return "radius-increase-not-allowed";
	case ECustomSafeZoneRuntimeFailure::MapProjectionUnavailable:
		return "map-projection-unavailable";
	case ECustomSafeZoneRuntimeFailure::InsufficientPhaseCapacity:
		return "insufficient-phase-capacity";
	case ECustomSafeZoneRuntimeFailure::MissingPhaseFields:
		return "missing-phase-fields";
	case ECustomSafeZoneRuntimeFailure::MissingIndicatorFields:
		return "missing-indicator-fields";
	case ECustomSafeZoneRuntimeFailure::MissingPhasePublisher:
		return "missing-phase-publisher";
	case ECustomSafeZoneRuntimeFailure::InvalidLegacyLocations:
		return "invalid-legacy-locations";
	default:
		return "unknown";
	}
}

#if defined(_DEBUG)
void CustomSafeZoneRuntime::RunMappingSelfTests()
{
	static bool bRan = false;
	if (bRan)
		return;
	bRan = true;

	constexpr int32 Start = 4;
	constexpr int32 Source = Start - 1;
	constexpr int32 Nodes = 3;
	assert(GeometryNodeIndexForPhase(Source, Nodes, 2) == -1);
	assert(GeometryNodeIndexForPhase(Source, Nodes, 3) == 0);
	assert(GeometryNodeIndexForPhase(Source, Nodes, 4) == 1);
	assert(GeometryNodeIndexForPhase(Source, Nodes, 5) == 2);
	assert(GeometryNodeIndexForPhase(Source, Nodes, 9) == 2);
	assert(TimingSourceNodeIndexForPhase(Start, Nodes, 3) == -1);
	assert(TimingSourceNodeIndexForPhase(Start, Nodes, 4) == 0);
	assert(TimingSourceNodeIndexForPhase(Start, Nodes, 5) == 1);
	assert(TimingSourceNodeIndexForPhase(Start, Nodes, 6) == -1);
	assert(TimingSourceNodeIndexForPhase(Start, Nodes, 9) == -1);
	assert(LogicalToRuntimeStartPhase(4, 23.40f) == 4);
	assert(LogicalToRuntimeStartPhase(4, 24.00f) == 7);
	assert(ResolveAuthoredDuration({}, 12.f) == 12.f);
	assert(ResolveAuthoredDuration(0.f, 12.f) == 0.f);
	assert(HasCapacityForPlan(Start, Nodes, 6));
	assert(!HasCapacityForPlan(Start, Nodes, 5));
	assert(EffectiveNodeCount(1, false) == 1);
	assert(EffectiveNodeCount(1, true) == 2);
	assert(HasCapacityForPlan(
		Start, EffectiveNodeCount(Nodes, false), 6));
	assert(!HasCapacityForPlan(
		Start, EffectiveNodeCount(Nodes, true), 6));
	assert(HasCapacityForPlan(
		Start, EffectiveNodeCount(Nodes, true), 7));

	FCustomSafeZoneSequence RadiusOrder;
	RadiusOrder.Nodes.resize(3);
	RadiusOrder.Nodes[0].RadiusCm = 1000.f;
	RadiusOrder.Nodes[1].RadiusCm = 900.f;
	RadiusOrder.Nodes[2].RadiusCm = 1100.f;
	assert(RadiusOrder.FindFirstRadiusIncreaseEdge().has_value());
	assert(*RadiusOrder.FindFirstRadiusIncreaseEdge() == 1);
	RadiusOrder.Nodes[2].RadiusCm = 900.f;
	assert(!RadiusOrder.FindFirstRadiusIncreaseEdge().has_value());

	const auto SavedVisibleStatus = GetStatus();
	FCustomSafeZoneRuntimeStatus EdgeStatus;
	EdgeStatus.Failure =
		ECustomSafeZoneRuntimeFailure::RadiusIncreaseNotAllowed;
	EdgeStatus.NodeCount = 3;
	EdgeStatus.bCloseFinalCircle = true;
	EdgeStatus.OffendingEdge = 1;
	PublishVisibleStatus(EdgeStatus);
	const auto PublishedEdgeStatus = GetStatus();
	assert(PublishedEdgeStatus.OffendingEdge == 1);
	assert(PublishedEdgeStatus.bCloseFinalCircle);
	EdgeStatus.Failure = ECustomSafeZoneRuntimeFailure::NotRequested;
	EdgeStatus.bCloseFinalCircle = false;
	EdgeStatus.OffendingEdge = -1;
	PublishVisibleStatus(EdgeStatus);
	assert(GetStatus().OffendingEdge == -1);
	PublishVisibleStatus(SavedVisibleStatus);

	FCompiledCustomSafeZonePlan FinalTimingOnly;
	FinalTimingOnly.Nodes.reserve(2);
	FinalTimingOnly.Nodes.push_back(
		{ FVector{}, 1000.f, {}, {} });
	FinalTimingOnly.Nodes.push_back(
		{ FVector{}, 900.f, 5.f, 6.f });
	assert(!HasAuthoredHoldTiming(FinalTimingOnly));
	assert(!HasAuthoredMoveTiming(FinalTimingOnly));

	// Final authored timing is ordinarily inert, but it becomes the outgoing
	// timing for the synthetic same-center radius-zero target when closure is
	// enabled. A one-circle sequence therefore becomes one valid transition.
	FCompiledCustomSafeZonePlan ClosingPlan;
	ClosingPlan.RuntimeStartPhase = Start;
	ClosingPlan.SourcePhase = Source;
	ClosingPlan.AuthoredNodeCount = 1;
	ClosingPlan.bCloseFinalCircle = true;
	ClosingPlan.Nodes.push_back(
		{ FVector(10.f, 20.f, 30.f), 900.f, 5.f, 6.f });
	ClosingPlan.Nodes.push_back(
		{ FVector(10.f, 20.f, 30.f), 0.f, {}, {} });
	assert(HasAuthoredHoldTiming(ClosingPlan));
	assert(HasAuthoredMoveTiming(ClosingPlan));
	assert(TimingSourceNodeIndexForPhase(
		Start, (int32)ClosingPlan.Nodes.size(), Start) == 0);
	assert(GeometryNodeIndexForPhase(
		Source, (int32)ClosingPlan.Nodes.size(), Source) == 0);
	assert(GeometryNodeIndexForPhase(
		Source, (int32)ClosingPlan.Nodes.size(), Start) == 1);
	assert(ClosingPlan.Nodes.back().Radius == 0.f);
	assert(ClosingPlan.Nodes.back().Center.X ==
		ClosingPlan.Nodes.front().Center.X);

	// Immutable publication/CAS/freeze regression coverage. Preserve whatever
	// profile initialization published before this hook ran.
	const auto OriginalSnapshot =
		FConfiguration::GetCustomSafeZoneSequenceSnapshot();
	const FCustomSafeZoneSequence OriginalSequence = OriginalSnapshot
		? *OriginalSnapshot
		: FCustomSafeZoneSequence{};
	const bool bOriginalMoving = OriginalSnapshot &&
		OriginalSnapshot->bMovingZoneEnabled;
	const bool bOriginalCustomSafeZone =
		FConfiguration::bCustomSafeZone.load(
			std::memory_order_acquire);
	FConfiguration::ReleaseCustomSafeZoneSequenceForMatch();
	assert(FConfiguration::SetCustomSafeZoneEnabled(true));

	FCustomSafeZoneSequence PublishedSequence;
	PublishedSequence.Nodes.resize(2);
	PublishedSequence.Nodes[0].Center = FVector(10.f, 20.f, 30.f);
	PublishedSequence.Nodes[0].RadiusCm = 20000.f;
	PublishedSequence.Nodes[1].Center = FVector(40.f, 50.f, 60.f);
	PublishedSequence.Nodes[1].RadiusCm = 15000.f;
	PublishedSequence.bCloseFinalCircle = true;
	assert(FConfiguration::PublishCustomSafeZoneSequence(
		PublishedSequence, true));
	auto FirstPublication =
		FConfiguration::GetCustomSafeZoneSequenceSnapshot();
	assert(FirstPublication->bCloseFinalCircle);
	FCustomSafeZoneSequence IncreasingSequence = *FirstPublication;
	IncreasingSequence.Nodes[1].RadiusCm = 25000.f;
	assert(!FConfiguration::PublishCustomSafeZoneSequence(
		IncreasingSequence, true));
	assert(FConfiguration::GetCustomSafeZoneSequenceSnapshot() ==
		FirstPublication);
	FCustomSafeZoneSequence EqualRadiusSequence = *FirstPublication;
	EqualRadiusSequence.Nodes[1].RadiusCm =
		EqualRadiusSequence.Nodes[0].RadiusCm;
	assert(FConfiguration::PublishCustomSafeZoneSequence(
		EqualRadiusSequence, true));
	PublishedSequence.Nodes[0].Center.X = 999.f;
	assert(FirstPublication->Nodes[0].Center.X == 10.f);
	assert(FConfiguration::CustomSafeZoneCenter.X == 10.f);
	assert(FConfiguration::CustomSafeZoneRadius.load() == 20000.f);

	FCustomSafeZoneSequence NewerSequence = *FirstPublication;
	NewerSequence.Nodes[0].Center.X = 11.f;
	assert(FConfiguration::PublishCustomSafeZoneSequence(
		NewerSequence, true));
	assert(!FConfiguration::PublishCustomSafeZoneSequenceIfCurrent(
		FirstPublication, PublishedSequence, true));
	auto AcceptedPublication =
		FConfiguration::GetCustomSafeZoneSequenceSnapshot();
	assert(FConfiguration::FreezeCustomSafeZoneSequenceForMatch(
		AcceptedPublication));
	assert(!FConfiguration::SetCustomSafeZoneEnabled(false));
	assert(!FConfiguration::PublishCustomSafeZoneSequence(
		PublishedSequence, true));
	assert(FConfiguration::GetCustomSafeZoneSequenceSnapshot() ==
		AcceptedPublication);

	FConfiguration::ReleaseCustomSafeZoneSequenceForMatch();
	FCustomSafeZoneSequence EmptySequence;
	EmptySequence.Nodes.clear();
	assert(!FConfiguration::PublishCustomSafeZoneSequence(
		EmptySequence, true));

	// Readers must observe a complete immutable publication while independent
	// writers serialize whole-sequence replacements.
	FCustomSafeZoneSequence ConcurrentBaseline;
	ConcurrentBaseline.Nodes.resize(2);
	ConcurrentBaseline.Nodes[0].Center.X = 1.f;
	ConcurrentBaseline.Nodes[0].RadiusCm = 20001.f;
	ConcurrentBaseline.Nodes[1].Center.X = -1.f;
	ConcurrentBaseline.Nodes[1].RadiusCm = 10001.f;
	assert(FConfiguration::PublishCustomSafeZoneSequence(
		ConcurrentBaseline, true));
	std::atomic_bool BeginConcurrentTest{ false };
	std::atomic_bool ConcurrentSnapshotFailed{ false };
	auto PublishRange = [&](int Begin, int End)
	{
		while (!BeginConcurrentTest.load(std::memory_order_acquire))
			std::this_thread::yield();
		for (int Marker = Begin; Marker < End; ++Marker)
		{
			FCustomSafeZoneSequence Candidate;
			Candidate.Nodes.resize(2);
			Candidate.Nodes[0].Center.X = (float)Marker;
			Candidate.Nodes[0].RadiusCm = 20000.f + Marker;
			Candidate.Nodes[1].Center.X = (float)-Marker;
			Candidate.Nodes[1].RadiusCm = 10000.f + Marker;
			if (!FConfiguration::PublishCustomSafeZoneSequence(
					std::move(Candidate), true))
			{
				ConcurrentSnapshotFailed.store(
					true, std::memory_order_release);
				return;
			}
		}
	};
	std::thread WriterA(PublishRange, 1, 65);
	std::thread WriterB(PublishRange, 65, 129);
	std::thread Reader([&]()
	{
		BeginConcurrentTest.store(true, std::memory_order_release);
		for (int Iteration = 0; Iteration < 1024; ++Iteration)
		{
			const auto View =
				FConfiguration::GetCustomSafeZoneSequenceSnapshot();
			if (!View || View->Nodes.size() != 2)
			{
				ConcurrentSnapshotFailed.store(
					true, std::memory_order_release);
				break;
			}
			const float Marker = (float)View->Nodes[0].Center.X;
			if (fabsf(View->Nodes[0].RadiusCm -
					(20000.f + Marker)) > 0.01f ||
				fabsf((float)View->Nodes[1].Center.X + Marker) > 0.01f ||
				fabsf(View->Nodes[1].RadiusCm -
					(10000.f + Marker)) > 0.01f)
			{
				ConcurrentSnapshotFailed.store(
					true, std::memory_order_release);
				break;
			}
		}
	});
	WriterA.join();
	WriterB.join();
	Reader.join();
	assert(!ConcurrentSnapshotFailed.load(
		std::memory_order_acquire));

	// Parent-disable and match-freeze share the same publication lock. Exactly
	// one side of each race is allowed to commit, so preflight can never freeze a moving
	// sequence whose parent option was concurrently disabled.
	for (int Iteration = 0; Iteration < 32; ++Iteration)
	{
		FConfiguration::ReleaseCustomSafeZoneSequenceForMatch();
		assert(FConfiguration::SetCustomSafeZoneEnabled(true));
		const auto RaceSnapshot =
			FConfiguration::GetCustomSafeZoneSequenceSnapshot();
		std::atomic_bool BeginFreezeRace{ false };
		bool bFreezeWon = false;
		bool bDisableWon = false;
		std::thread Freezer([&]()
		{
			while (!BeginFreezeRace.load(std::memory_order_acquire))
				std::this_thread::yield();
			bFreezeWon =
				FConfiguration::FreezeCustomSafeZoneSequenceForMatch(
					RaceSnapshot);
		});
		std::thread Disabler([&]()
		{
			while (!BeginFreezeRace.load(std::memory_order_acquire))
				std::this_thread::yield();
			bDisableWon =
				FConfiguration::SetCustomSafeZoneEnabled(false);
		});
		BeginFreezeRace.store(true, std::memory_order_release);
		Freezer.join();
		Disabler.join();
		assert(bFreezeWon != bDisableWon);
	}
	FConfiguration::ReleaseCustomSafeZoneSequenceForMatch();
	assert(FConfiguration::PublishCustomSafeZoneSequence(
		OriginalSequence, bOriginalMoving));
	assert(FConfiguration::SetCustomSafeZoneEnabled(
		bOriginalCustomSafeZone));
}
#endif
