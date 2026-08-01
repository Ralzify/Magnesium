#pragma once

#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/FortGameStateAthena.h"
#include "../../../SDK/Engine.h"
#include "../../../SDK/Offsets.h"
#include "../../../SDK/Core.h"
#include "Configuration.h"
#include "GUI.h"
#include "Utils.h"

#include <atomic>
#include <cmath>
#include <unordered_map>

namespace Calendar
{
	static inline bool HasSnowMap()
	{
		return VersionInfo.FortniteVersion >= 7.10 && VersionInfo.FortniteVersion < 7.40 || VersionInfo.FortniteVersion == 11.3 || VersionInfo.FortniteVersion == 15.10 || std::floor(VersionInfo.FortniteVersion) == 19;
	}

	// Seasons do not agree on what "snow value" means. The Season 7 and
	// Winterfest setups blend the map along an authored curve and take that
	// curve's time (0..1), while the Chapter 3+ surface-coverage systems take a
	// discrete progression phase. GetSnowVersionModel describes which shape the
	// running build wants so callers can present and clamp the right control.
	enum class ESnowValueModel
	{
		Unsupported,
		Alpha,
		Phase,
	};

	struct FSnowPreset
	{
		const char* Label;
		float Value;
	};

	struct FSnowVersionModel
	{
		ESnowValueModel Model = ESnowValueModel::Unsupported;
		float Min = 0.f;
		float Max = 1.f;
		const FSnowPreset* Presets = nullptr;
		int PresetCount = 0;
		const char* Note = "";
	};

	static inline FSnowVersionModel GetSnowVersionModel()
	{
		// Keyframe times of the Season 7 snow curve. The depths they blend to
		// are the second half of the pairs in GetFullSnowMapValue.
		static const FSnowPreset SeasonSevenPresets[] = {
			{ "Clear", 0.f },
			{ "Light", 0.68104035f },
			{ "Heavy", 0.9632137f },
			{ "Full", 1.f },
		};

		// Winterfest 2019/2020 (BP_ApolloSnowSetup) keyframe times.
		static const FSnowPreset ApolloPresets[] = {
			{ "Clear", 0.f },
			{ "Light", 0.5f },
			{ "Heavy", 0.75f },
			{ "Full", 1.f },
		};

		static const FSnowPreset PhasePresets[] = {
			{ "Clear", 0.f },
			{ "Half", 4.f },
			{ "Full", 8.f },
		};

		const double Version = VersionInfo.FortniteVersion;

		if (Version == 7.10 || Version == 7.20 || Version == 7.30)
		{
			return { ESnowValueModel::Alpha, 0.f, 1.f, SeasonSevenPresets,
				sizeof(SeasonSevenPresets) / sizeof(SeasonSevenPresets[0]),
				"Season 7 snow setup. The value is a position along the season's snow curve." };
		}

		if (Version == 11.30 || Version == 11.31 || Version == 15.10)
		{
			return { ESnowValueModel::Alpha, 0.f, 1.f, ApolloPresets,
				sizeof(ApolloPresets) / sizeof(ApolloPresets[0]),
				"Winterfest snow setup. The value is a position along the season's snow curve." };
		}

		if (Version == 19.01)
		{
			return { ESnowValueModel::Phase, 0.f, 8.f, PhasePresets,
				sizeof(PhasePresets) / sizeof(PhasePresets[0]),
				"Chapter 3 snow progression. Phase 0 is a clear map, phase 8 is fully covered." };
		}

		if (Version == 28.01)
		{
			return { ESnowValueModel::Phase, 0.f, 8.f, PhasePresets,
				sizeof(PhasePresets) / sizeof(PhasePresets[0]),
				"Winterfest 2023. Driven through the progression phase entry point found on the map's own snow actor." };
		}

		return {};
	}

	static inline bool HasSnowControls()
	{
		return GetSnowVersionModel().Model != ESnowValueModel::Unsupported;
	}

	static inline float ClampSnowValue(float NewValue)
	{
		const FSnowVersionModel Model = GetSnowVersionModel();

		if (NewValue < Model.Min)
			NewValue = Model.Min;
		else if (NewValue > Model.Max)
			NewValue = Model.Max;

		// Phases are indices. SetSnow marshals the value as a float either way,
		// so round here rather than handing the blueprint phase 3.7.
		if (Model.Model == ESnowValueModel::Phase)
			NewValue = std::round(NewValue);

		return NewValue;
	}

	// The entry points a snow setup can expose. Resolving these against the
	// actor itself rather than a hardcoded package path is what lets one code
	// path drive every season's blueprint.
	static inline UFunction* FindSnowFunction(const UObject* SnowSetup)
	{
		if (!SnowSetup)
			return nullptr;

		static const char* const FunctionNames[] = {
			"SetSnow",
			"SetSnowProgressionPhase",
			"SetSnowFall",
		};

		for (auto FunctionName : FunctionNames)
		{
			if (auto Function = SnowSetup->GetFunction(FunctionName))
				return Function;
		}

		return nullptr;
	}

	// 7.20 and 28.01 ship snow setups that none of the hardcoded blueprint
	// paths below resolve. Rather than guessing at more package paths, find the
	// actor by the one thing that stays stable across seasons: it is a live
	// actor whose class exposes a snow entry point. The reflection walk is
	// memoized per class, so this costs one pass over the object array.
	static inline UObject* ScanForSnowSetup()
	{
		auto ActorClass = AActor::StaticClass();

		if (!ActorClass)
			return nullptr;

		// Same reachability filter TUObjectArray::FindObject uses.
		const int32 SkipFlags = Offsets::bEncryptedObjects ? 0x10200000 : 0x20;
		const int32 ObjectCount = TUObjectArray::Num();

		std::unordered_map<const UClass*, bool> DrivesSnow;

		for (int32 Index = 0; Index < ObjectCount; ++Index)
		{
			auto Item = TUObjectArray::GetItemByIndex(Index);

			if (!Item || (Item->GetFlags() & SkipFlags))
				continue;

			auto Object = const_cast<UObject*>(Item->GetObject());

			if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA(ActorClass))
				continue;

			auto Known = DrivesSnow.find(Object->Class);

			if (Known == DrivesSnow.end())
				Known = DrivesSnow.emplace(Object->Class, FindSnowFunction(Object) != nullptr).first;

			if (Known->second)
				return Object;
		}

		return nullptr;
	}

	// Resolving the snow setup class costs a pass over the object array, so it
	// is cached - but only for the lifetime of one world, since a discovered
	// blueprint class can belong to a level that later streams out.
	inline UClass* GSnowSetupClass = nullptr;

	static inline UObject* GetSnowSetup()
	{
		auto World = UWorld::GetWorld();

		if (!World)
			return nullptr;

		UClass*& SnowSetupClass = GSnowSetupClass;

		if (!SnowSetupClass)
		{
			SnowSetupClass = const_cast<UClass*>(FindObject<UClass>(L"/Game/Athena/Environments/Landscape/Blueprints/BP_SnowSetup.BP_SnowSetup_C"));

			if (!SnowSetupClass)
				SnowSetupClass = const_cast<UClass*>(FindObject<UClass>(L"/Game/Athena/Apollo/Environments/Blueprints/CalendarEvents/BP_ApolloSnowSetup.BP_ApolloSnowSetup_C"));
		}

		if (!SnowSetupClass)
		{
			auto ArtemisActor = FindObject<AActor>(L"/SpecialSurfaceCoverage/Maps/SpecialSurfaceCoverage_Artemis_Terrain_LS_Parent_Overlay.SpecialSurfaceCoverage_Artemis_Terrain_LS_Parent_Overlay.PersistentLevel.BP_Artemis_S19Progression_C_0");

			if (ArtemisActor)
				return const_cast<AActor*>(ArtemisActor);
		}

		if (SnowSetupClass)
		{
			auto Actors = UGameplayStatics::GetAllActorsOfClass(World, SnowSetupClass);
			auto Found = Actors.Num() > 0 ? Actors[0] : nullptr;
			Actors.Free();

			if (Found)
				return Found;
		}

		// Nothing hardcoded matched this build. Fall back to discovery and keep
		// the class so later calls take the cheap path above.
		if (auto Discovered = ScanForSnowSetup())
		{
			SnowSetupClass = Discovered->Class;
			return Discovered;
		}

		return nullptr;
	}

	static inline float GetFullSnowMapValue()
	{
		if (VersionInfo.FortniteVersion == 7.10 || VersionInfo.FortniteVersion == 7.30)
		{
			std::vector<std::pair<float, float>> TimeAndValues = { { 0, 1.2f}, { 0.68104035f, 4.6893263f }, { 0.9632137f, 10.13335f }, { 1.0f, 15.0f } };
			// 1.2
			// 4.6893263
			// 10.13335
			// 15;
			return TimeAndValues[3].first;
		}
		else if (VersionInfo.FortniteVersion == 11.31 || VersionInfo.FortniteVersion == 15.10)
		{
			std::vector<std::pair<float, float>> TimeAndValues = { { 0, 0.0f }, { 0.5f, 0.35f }, { 0.75f, 0.5f }, { 1.0f, 1.0f } };
			// 0
			// 0.35
			// 0.5
			// 1
			return TimeAndValues[3].first;
		}
		else if (VersionInfo.FortniteVersion == 19.01 || VersionInfo.FortniteVersion == 19.10)
		{
			std::vector<int> Values = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };

			return Values[6];
		}

		return -1;
	}

	static inline void SnowFog()
	{
		auto SnowSetup = GetSnowSetup();

		if (SnowSetup)
		{
			static auto OnReady1 = FindObject<UFunction>(L"/Game/Athena/Apollo/Environments/Blueprints/CalendarEvents/BP_ApolloSnowSetup.BP_ApolloSnowSetup_C.OnReady_E426AA7F4F2319EA06FBA2B9905F0B24");
			static auto OnReady2 = FindObject<UFunction>(L"/Game/Athena/Apollo/Environments/Blueprints/CalendarEvents/BP_ApolloSnowSetup.BP_ApolloSnowSetup_C.OnReady_0A511B314AE165C51798519FB84738B8");
			static auto RefreshMapLocs = FindObject<UFunction>(L"/Game/Athena/Apollo/Environments/Blueprints/CalendarEvents/BP_ApolloSnowSetup.BP_ApolloSnowSetup_C:RefreshMapLocations");

			auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
			auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;

			auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

			struct
			{
				UObject* GameState;
				UObject* CurrentPlaylist;
				FGameplayTagContainer PlaylistContextTags;
			} OnReadyParams { GameState, (UObject*)Playlist, FGameplayTagContainer() };

			SnowSetup->ProcessEvent(const_cast<UFunction*>(OnReady1), &OnReadyParams);
			SnowSetup->ProcessEvent(const_cast<UFunction*>(OnReady2), &OnReadyParams);
			SnowSetup->ProcessEvent(const_cast<UFunction*>(RefreshMapLocs), nullptr);
		}
	}

	static inline void SetSnowfall(float NewValue)
	{
		static auto SetSnowfallFn = FindObject<UFunction>(L"/Game/Athena/Apollo/Environments/Blueprints/CalendarEvents/BP_ApolloSnowSetup.BP_ApolloSnowSetup_C.SetSnowFall");

		auto SnowSetup = GetSnowSetup();

		if (SetSnowfallFn && SnowSetup)
		{
			SnowSetup->ProcessEvent(const_cast<UFunction*>(SetSnowfallFn), &NewValue);
		}
	}

	// Returns whether the map actually had a snow setup to drive.
	static inline bool SetSnow(float NewValue)
	{
		auto SnowSetup = GetSnowSetup();

		// Resolved off the actor instead of a package path: every season names
		// this blueprint differently, but they all hang the entry point on the
		// snow actor's own class.
		auto SetSnowFn = FindSnowFunction(SnowSetup);

		if (!SnowSetup || !SetSnowFn)
		{
			printf("Failed to find SnowSetup or SetSnowFn!");
			return false;
		}

		if (VersionInfo.EngineVersion >= 5.00)
			NewValue = (int)NewValue;

		SnowSetup->ProcessEvent(SetSnowFn, &NewValue);

		printf("Called SetSnow!");

		if (NewValue != -1 && VersionInfo.FortniteVersion >= 5.00 && !FindGIsClient())
		{
			// Only the surface coverage progression actor has this; on the
			// seasons that do not, ProcessEvent would fault on a null function.
			if (auto UpdateSnowVisualsOnClientFn = SnowSetup->GetFunction("UpdateSnowVisualsOnClient"))
			{
				SnowSetup->ProcessEvent(UpdateSnowVisualsOnClientFn, nullptr);
				printf("Called UpdateSnowVisualsOnClientFn!");
			}
		}

		return true;
	}

	// ------------------------------------------------------------------
	// Menu -> game thread
	//
	// The menu renders on its own thread and must not touch Unreal objects, so
	// it only publishes the value it wants. TickSnow consumes the request from
	// the game-thread net driver tick, which is also where the start-of-match
	// value gets applied once the map has streamed its snow actor in.
	// ------------------------------------------------------------------

	enum class ESnowStatus
	{
		Idle,
		Pending,
		Applied,
		SetupMissing,
	};

	// These cross threads, so they must have external linkage - `static inline`
	// would hand every translation unit its own copy of the request.
	inline std::atomic<float> GRequestedSnowValue{ 0.f };
	inline std::atomic_bool GHasSnowRequest{ false };
	inline std::atomic<float> GAppliedSnowValue{ -1.f };
	inline std::atomic_int GSnowStatus{ static_cast<int>(ESnowStatus::Idle) };

	// Owned by TickSnow, which only ever runs on the game thread.
	inline void* GSnowWorld = nullptr;
	inline unsigned long long GNextSnowAttemptMs = 0;
	inline int GAutoApplyAttemptsLeft = 0;
	inline bool GSnowMatchStarted = false;
	inline bool GSnowRefreshPending = false;

	constexpr unsigned long long SnowRetryIntervalMs = 2000;
	// One automatic window opens when the world comes up and a second one when
	// the match starts, each ~30s of applies at SnowRetryIntervalMs. A single
	// apply at world load does not hold: the map's own snow setup is still
	// initialising and can overwrite it, and nothing pushed before the first
	// client connects reaches the players who join afterwards.
	constexpr int SnowAutoApplyAttempts = 15;

	static inline void RequestSnow(float NewValue)
	{
		GRequestedSnowValue.store(NewValue, std::memory_order_release);
		GSnowStatus.store(static_cast<int>(ESnowStatus::Pending), std::memory_order_release);
		GHasSnowRequest.store(true, std::memory_order_release);
	}

	static inline ESnowStatus GetSnowStatus()
	{
		return static_cast<ESnowStatus>(GSnowStatus.load(std::memory_order_acquire));
	}

	static inline float GetAppliedSnowValue()
	{
		return GAppliedSnowValue.load(std::memory_order_acquire);
	}

	// What the world should currently be showing: whatever was last applied,
	// falling back to the configured start-of-match value.
	static inline float GetEffectiveSnowValue()
	{
		const float Applied = GAppliedSnowValue.load(std::memory_order_acquire);

		return Applied >= 0.f
			? Applied
			: FConfiguration::SnowValue.load(std::memory_order_acquire);
	}

	// Called when a player acknowledges possession of their pawn. Snow pushed
	// before a client is in the world does not reach that client, so the value
	// has to be re-sent once the player is actually playing - that is the whole
	// reason a single apply at match start never showed up. The push itself is
	// left to the next tick so a bus full of players collapses into one.
	static inline void RequestSnowRefreshForPlayer()
	{
		if (HasSnowControls())
			GSnowRefreshPending = true;
	}

	static inline void TickSnow()
	{
		auto World = UWorld::GetWorld();

		if (!World)
			return;

		if (GSnowWorld != World)
		{
			// A new world means a new snow actor, and the start-of-match value
			// gets another chance to land.
			GSnowWorld = World;
			GSnowSetupClass = nullptr;
			GNextSnowAttemptMs = 0;
			GAutoApplyAttemptsLeft = SnowAutoApplyAttempts;
			GSnowMatchStarted = false;
			GAppliedSnowValue.store(-1.f, std::memory_order_release);
			GSnowStatus.store(static_cast<int>(ESnowStatus::Idle), std::memory_order_release);
		}

		if (!HasSnowControls())
			return;

		if (!GSnowMatchStarted &&
			GUI::gsStatus.load(std::memory_order_acquire) >= StartedMatch)
		{
			// Reopen the window at the bus. This is the point the automatic
			// value has to survive: the map finishes its own snow setup around
			// here, and it is the first moment every player is connected to
			// receive the change.
			GSnowMatchStarted = true;
			GNextSnowAttemptMs = 0;
			GAutoApplyAttemptsLeft = SnowAutoApplyAttempts;
		}

		const bool bPendingRefresh = GSnowRefreshPending;
		GSnowRefreshPending = false;

		const bool bRequested = GHasSnowRequest.load(std::memory_order_acquire);
		const bool bAutoApply =
			!bRequested &&
			GAutoApplyAttemptsLeft > 0 &&
			FConfiguration::bSnowOnMatchStart.load(std::memory_order_acquire);
		// A refresh only means "send it again", so it needs something to have
		// decided what the snow should be in the first place.
		const bool bRefresh =
			!bRequested &&
			!bAutoApply &&
			bPendingRefresh &&
			(GAppliedSnowValue.load(std::memory_order_acquire) >= 0.f ||
				FConfiguration::bSnowOnMatchStart.load(std::memory_order_acquire));

		if (!bRequested && !bAutoApply && !bRefresh)
			return;

		// Locating the snow actor can cost a pass over the object array, so
		// never attempt it more than once per interval. A refresh is exempt: it
		// has to land while the player who triggered it is in the world.
		const unsigned long long NowMs = GetTickCount64();

		if (!bRefresh && NowMs < GNextSnowAttemptMs)
			return;

		GNextSnowAttemptMs = NowMs + SnowRetryIntervalMs;

		const float NewValue = ClampSnowValue(
			bRequested
				? GRequestedSnowValue.load(std::memory_order_acquire)
				: (bAutoApply
					? FConfiguration::SnowValue.load(std::memory_order_acquire)
					: GetEffectiveSnowValue()));

		const bool bApplied = SetSnow(NewValue);

		if (bRequested)
		{
			// An explicit press owns the value for the rest of this world, so
			// the automatic window stops competing with it.
			GHasSnowRequest.store(false, std::memory_order_release);
			GSnowMatchStarted = true;
			GAutoApplyAttemptsLeft = 0;
		}
		else if (bAutoApply)
		{
			// Every attempt in the window re-applies, not just the ones before
			// the first success - whatever the map does to the snow setup after
			// we get there has to be corrected.
			--GAutoApplyAttemptsLeft;
		}

		if (bApplied)
		{
			GAppliedSnowValue.store(NewValue, std::memory_order_release);
			GSnowStatus.store(static_cast<int>(ESnowStatus::Applied), std::memory_order_release);
		}
		else if (bRequested ||
			(bAutoApply &&
				GAutoApplyAttemptsLeft <= 0 &&
				GAppliedSnowValue.load(std::memory_order_acquire) < 0.f))
		{
			// The map may still be streaming its snow actor in, so only report
			// a miss once the window is out of retries. A button press reports
			// back immediately rather than leaving the menu on "Applying".
			GSnowStatus.store(static_cast<int>(ESnowStatus::SetupMissing), std::memory_order_release);
		}
	}

	// water level stuff maybe idk
}