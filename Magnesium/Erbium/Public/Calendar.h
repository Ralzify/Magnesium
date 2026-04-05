#pragma once

#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/FortGameStateAthena.h"
#include "../../../SDK/Engine.h"
#include "../../../SDK/Offsets.h"
#include "../../../SDK/Core.h"
#include "Utils.h"

namespace Calendar
{
	static inline bool HasSnowMap()
	{
		return VersionInfo.FortniteVersion >= 7.10 && VersionInfo.FortniteVersion < 7.40 || VersionInfo.FortniteVersion == 11.3 || VersionInfo.FortniteVersion == 15.10 || std::floor(VersionInfo.FortniteVersion) == 19;
	}

	static inline UObject* GetSnowSetup()
	{
		auto World = UWorld::GetWorld();

		if (!World)
			return nullptr;

		static UClass* SnowSetupClass = nullptr;

		if (!SnowSetupClass)
		{
			SnowSetupClass = const_cast<UClass*>(FindObject<UClass>(L"/Game/Athena/Environments/Landscape/Blueprints/BP_SnowSetup.BP_SnowSetup_C"));

			if (!SnowSetupClass)
				SnowSetupClass = const_cast<UClass*>(FindObject<UClass>(L"/Game/Athena/Apollo/Environments/Blueprints/CalendarEvents/BP_ApolloSnowSetup.BP_ApolloSnowSetup_C"));
		}

		if (!SnowSetupClass)
		{
			auto ArtemisActor = FindObject<AActor>(L"/SpecialSurfaceCoverage/Maps/SpecialSurfaceCoverage_Artemis_Terrain_LS_Parent_Overlay.SpecialSurfaceCoverage_Artemis_Terrain_LS_Parent_Overlay.PersistentLevel.BP_Artemis_S19Progression_C_0");
			return const_cast<AActor*>(ArtemisActor);
		}

		auto Actors = UGameplayStatics::GetAllActorsOfClass(World, SnowSetupClass);

		if (Actors.Num() > 0)
			return Actors[0];

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

	static inline void SetSnow(float NewValue)
	{
		static auto SetSnowFn = FindObject<UFunction>(L"/Game/Athena/Apollo/Environments/Blueprints/CalendarEvents/BP_ApolloSnowSetup.BP_ApolloSnowSetup_C.SetSnow") ? FindObject<UFunction>(L"/Game/Athena/Apollo/Environments/Blueprints/CalendarEvents/BP_ApolloSnowSetup.BP_ApolloSnowSetup_C.SetSnow") : FindObject<UFunction>(L"/Game/Athena/Environments/Landscape/Blueprints/BP_SnowSetup.BP_SnowSetup_C.SetSnow") ? FindObject<UFunction>(L"/Game/Athena/Environments/Landscape/Blueprints/BP_SnowSetup.BP_SnowSetup_C.SetSnow") : FindObject<UFunction>("/SpecialSurfaceCoverage/Items/BP_Artemis_S19Progression.BP_Artemis_S19Progression_C.SetSnowProgressionPhase");

		auto SnowSetup = GetSnowSetup();

		if (!SnowSetup || !SetSnowFn)
		{
			printf("Failed to find SnowSetup or SetSnowFn!");
			return;
		}

		if (SnowSetup && SetSnowFn)
		{
			if (VersionInfo.EngineVersion >= 5.00)
				NewValue = (int)NewValue;

			SnowSetup->ProcessEvent(const_cast<UFunction*>(SetSnowFn), &NewValue);

			printf("Called SetSnow!");

			if (NewValue != -1 && VersionInfo.FortniteVersion >= 5.00 && !FindGIsClient())
			{
				auto UpdateSnowVisualsOnClientFn = FindObject<UFunction>("/SpecialSurfaceCoverage/Items/BP_Artemis_S19Progression.BP_Artemis_S19Progression_C.UpdateSnowVisualsOnClient");
				SnowSetup->ProcessEvent(const_cast<UFunction*>(UpdateSnowVisualsOnClientFn), nullptr);
				printf("Called UpdateSnowVisualsOnClientFn!");
			}
		}
		else
		{
			printf("Failed to find SnowSetup!");
		}
	}

	// water level stuff maybe idk
}