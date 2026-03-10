#pragma once
#include "../../pch.h"

struct FEventFunction
{
    bool bIsLoaderFunction;
    const wchar_t* FunctionPath;
};

struct FEvent
{
    const wchar_t* LoaderClass;
    const wchar_t* ScriptingClass;
    std::vector<FEventFunction> EventFunctions;
    double EventVersion;
    const wchar_t* LoaderFuncPath;
	const wchar_t* PlaylistPath;
};

class Events
{
public:
#ifndef CLIENT
	static inline std::vector<FEvent> EventsArray =
	{
		FEvent
		{
			nullptr,
			L"/Game/Athena/Maps/Test/Events/BP_GeodeScripting.BP_GeodeScripting_C",
			{
				FEventFunction{ false, L"/Game/Athena/Maps/Test/Events/BP_GeodeScripting.BP_GeodeScripting_C.LaunchSequence" }
			},
			4.5,
			nullptr,
			L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo"
		},
		FEvent
		{
			L"/Game/Athena/Prototype/Blueprints/Cube/CUBE.CUBE_C",
			L"/Game/Athena/Events/BP_Athena_Event_Components.BP_Athena_Event_Components_C",
			{
				FEventFunction{ false, L"/Game/Athena/Events/BP_Athena_Event_Components.BP_Athena_Event_Components_C.Final" },
				FEventFunction{ true, L"/Game/Athena/Prototype/Blueprints/Cube/CUBE.CUBE_C.Final" }
			},
			5.30,
			nullptr,
			L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo"
		},
		FEvent
		{
			L"/Game/Athena/Prototype/Blueprints/Island/BP_Butterfly.BP_Butterfly_C",
			nullptr,
			{
				FEventFunction{ true, L"/Game/Athena/Prototype/Blueprints/Island/BP_Butterfly.BP_Butterfly_C.ButterflySequence" }
			},
			6.21,
			L"/Game/Athena/Prototype/Blueprints/Island/BP_Butterfly.BP_Butterfly_C.LoadButterflySublevel",
			L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo"
		},
		FEvent
		{
			L"/Game/Athena/Prototype/Blueprints/Mooney/BP_MooneyLoader.BP_MooneyLoader_C",
			L"/Game/Athena/Prototype/Blueprints/Mooney/BP_MooneyScripting.BP_MooneyScripting_C",
			{
				FEventFunction{ false, L"/Game/Athena/Prototype/Blueprints/Mooney/BP_MooneyScripting.BP_MooneyScripting_C.OnReady_9968C1F648044523426FE198948B0CC9" },
				FEventFunction{ false, L"/Game/Athena/Prototype/Blueprints/Mooney/BP_MooneyScripting.BP_MooneyScripting_C.BeginIceKingEvent" },
			},
			7.20,
			L"/Game/Athena/Prototype/Blueprints/Mooney/BP_MooneyLoader.BP_MooneyLoader_C.LoadMap",
			L"/Game/Athena/Playlists/Deimos/Playlist_Deimos_Solo_Winter.Playlist_Deimos_Solo_Winter"
		},
		FEvent
		{
			nullptr,
			L"/Game/Athena/Environments/Festivus/Blueprints/BP_FestivusManager.BP_FestivusManager_C",
			{
				FEventFunction{ false, L"/Game/Athena/Environments/Festivus/Blueprints/BP_FestivusManager.BP_FestivusManager_C.OnReady_EE7676604ADFD92D7B2972AC0ABD4BB8" },
				FEventFunction{ false, L"/Game/Athena/Environments/Festivus/Blueprints/BP_FestivusManager.BP_FestivusManager_C.ServerPlayFestivus" },
			},
			7.30,
			nullptr,
			L"/Game/Athena/Playlists/Music/Playlist_Music_High.Playlist_Music_High"
		},
		FEvent
		{
			nullptr,
			L"/Game/Athena/Prototype/Blueprints/White/BP_SnowScripting.BP_SnowScripting_C",
			{
				FEventFunction{ false, L"/Game/Athena/Prototype/Blueprints/White/BP_SnowScripting.BP_SnowScripting_C.FinalSequence" }
			},
			8.51,
			L"/Game/Athena/Prototype/Blueprints/White/BP_SnowScripting.BP_SnowScripting_C.LoadSnowLevel",
			L"/Game/Athena/Playlists/Music/Playlist_Music_High.Playlist_Music_High"
		},
		FEvent
		{
			L"/Game/Athena/Prototype/Blueprints/Cattus/BP_CattusDoggus_Scripting.BP_CattusDoggus_Scripting_C",
			nullptr,
			{
				FEventFunction{ true, L"/Game/Athena/Prototype/Blueprints/Cattus/BP_CattusDoggus_Scripting.BP_CattusDoggus_Scripting_C.OnReady_C11CA7624A74FBAEC54753A3C2BD4506" },
				FEventFunction{ true, L"/Game/Athena/Prototype/Blueprints/Cattus/BP_CattusDoggus_Scripting.BP_CattusDoggus_Scripting_C.startevent" }
			},
			9.40,
			L"/Game/Athena/Prototype/Blueprints/Cattus/BP_CattusDoggus_Scripting.BP_CattusDoggus_Scripting_C.LoadCattusLevel",
			L"/Game/Athena/Playlists/Music/Playlist_Music_High.Playlist_Music_High"
		},
		FEvent
		{
			L"/Game/Athena/Prototype/Blueprints/Cattus/BP_CattusDoggus_Scripting.BP_CattusDoggus_Scripting_C",
			nullptr,
			{
				FEventFunction{ true, L"/Game/Athena/Prototype/Blueprints/Cattus/BP_CattusDoggus_Scripting.BP_CattusDoggus_Scripting_C.OnReady_C11CA7624A74FBAEC54753A3C2BD4506" },
				FEventFunction{ true, L"/Game/Athena/Prototype/Blueprints/Cattus/BP_CattusDoggus_Scripting.BP_CattusDoggus_Scripting_C.startevent" }
			},
			9.41,
			L"/Game/Athena/Prototype/Blueprints/Cattus/BP_CattusDoggus_Scripting.BP_CattusDoggus_Scripting_C.LoadCattusLevel",
			L"/Game/Athena/Playlists/Music/Playlist_Music_High.Playlist_Music_High"
		},
		FEvent
		{
			nullptr,
			L"/Game/Athena/Prototype/Blueprints/NightNight/BP_NightNight_Scripting.BP_NightNight_Scripting_C",
			{
				FEventFunction{ false, L"/Game/Athena/Prototype/Blueprints/NightNight/BP_NightNight_Scripting.BP_NightNight_Scripting_C.OnReady_D0847F7B4E80F01E77156AA4E7131AF6" },
				FEventFunction{ false, L"/Game/Athena/Prototype/Blueprints/NightNight/BP_NightNight_Scripting.BP_NightNight_Scripting_C.startevent" }
			},
			10.40,
			L"/Game/Athena/Prototype/Blueprints/NightNight/BP_NightNight_Scripting.BP_NightNight_Scripting_C.LoadNightNightLevel",
			L"/Game/Athena/Playlists/Music/Playlist_Music_High.Playlist_Music_High"
		},
		FEvent
		{
			nullptr,
			L"/Game/Athena/Events/NewYear/BP_NewYearTimer.BP_NewYearTimer_C",
			{
				FEventFunction{ false, L"/Game/Athena/Events/NewYear/BP_NewYearTimer.BP_NewYearTimer_C.startNYE" },
				FEventFunction{ false, L"/Game/Athena/Events/NewYear/BP_NewYearTimer.BP_NewYearTimer_C.RollStormOut" },
				FEventFunction{ false, L"/Game/Athena/Events/NewYear/BP_NewYearTimer.BP_NewYearTimer_C.TimeOfDaySetup" }
			},
			11.31,
			nullptr,
			nullptr
		},
		FEvent
		{
			L"/CycloneJerky/Gameplay/BP_Jerky_Loader.BP_Jerky_Loader_C",
			L"/CycloneJerky/Gameplay/BP_Jerky_Scripting.BP_Jerky_Scripting_C",
			{
				//FEventFunction{ false, L"/CycloneJerky/Gameplay/BP_Jerky_Scripting.BP_Jerky_Scripting_C.OnReady_093B6E664C060611B28F79B5E7052A39" },
				//FEventFunction{ true, L"/CycloneJerky/Gameplay/BP_Jerky_Loader.BP_Jerky_Loader_C.OnReady_7FE9744D479411040654F5886C078D08" },
				FEventFunction{ true, L"/CycloneJerky/Gameplay/BP_Jerky_Loader.BP_Jerky_Loader_C.startevent" }
			},
			12.41,
			nullptr,
			L"/Game/Athena/Playlists/Music/Playlist_Music_High.Playlist_Music_High"
		},
		FEvent
		{
			L"/Fritter/BP_Fritter_Loader.BP_Fritter_Loader_C",
			L"/Fritter/BP_Fritter_Script.BP_Fritter_Script_C",
			{
				FEventFunction{ false, L"/Fritter/BP_Fritter_Script.BP_Fritter_Script_C.OnReady_ACE66C28499BF8A59B3D88A981DDEF41" },
				FEventFunction{ true, L"/Fritter/BP_Fritter_Loader.BP_Fritter_Loader_C.OnReady_1216203B4B63E3DFA03042A62380A674" },
				FEventFunction{ true, L"/Fritter/BP_Fritter_Loader.BP_Fritter_Loader_C.startevent" }
			},
			12.61,
			nullptr,
			L"/Game/Athena/Playlists/Fritter/Playlist_Fritter_High.Playlist_Fritter_High"
		},
		FEvent
		{
			L"/Junior/Blueprints/BP_Junior_Loader.BP_Junior_Loader_C",
			L"/Junior/Blueprints/BP_Junior_Scripting.BP_Junior_Scripting_C",
			{
				FEventFunction{ false, L"/Junior/Blueprints/BP_Event_Master_Scripting.BP_Event_Master_Scripting_C.OnReady_872E6C4042121944B78EC9AC2797B053" },
				FEventFunction{ false, L"/Junior/Blueprints/BP_Junior_Scripting.BP_Junior_Scripting_C.startevent" }
			},
			14.60,
			L"/Junior/Blueprints/BP_Junior_Loader.BP_Junior_Loader_C.LoadJuniorLevel",
			L"/Game/Athena/Playlists/Music/Playlist_Junior_32.Playlist_Junior_32"
		},
		FEvent
		{
			nullptr,
			L"/Buffet/Gameplay/Blueprints/Buffet_SpecialEventScript.Buffet_SpecialEventScript_C",
			{
				FEventFunction{ false, L"/Script/SpecialEventGameplayRuntime.SpecialEventScript.StartEventAtIndex" }
			},
			17.30,
			nullptr,
			L"/BuffetPlaylist/Playlist/Playlist_Buffet.Playlist_Buffet"
		},
		FEvent
		{
			nullptr,
			L"/Kiwi/Gameplay/Kiwi_EventScript.Kiwi_EventScript_C",
			{
				FEventFunction{ false, L"/Script/SpecialEventGameplayRuntime.SpecialEventScript.StartEventAtIndex" }
			},
			17.50,
			nullptr,
			L"/KiwiPlaylist/Playlists/Playlist_Kiwi.Playlist_Kiwi"
		},
		FEvent
		{
			nullptr,
			L"/Guava/Gameplay/BP_Guava_SpecialEventScript.BP_Guava_SpecialEventScript_C",
			{
				FEventFunction{ false, L"/Script/SpecialEventGameplayRuntime.SpecialEventScript.StartEventAtIndex" }
			},
			18.40,
			nullptr,
			L"/GuavaPlaylist/Playlist/Playlist_Guava.Playlist_Guava"
		},
		FEvent
		{
			nullptr,
			L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C",
			{
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.OnReady_9EF500F248D546EAEE76C5A03688EA2E" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.CheatPulse" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.MultiCheatPulse" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.PlayPulseActivateVFX" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.PlayPulseActivateSFX" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.StartTowerPulse" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.SetActivatePulseTrue" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.StartPulseEvents" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.UpdateEventActiveStatus" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.PlayPulseActivationCosmetics" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.SetTimerForNextPulse" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.GetPulseRadiusAtLevel" },
				FEventFunction{ false, L"/ResolveDDMachine/Gameplay/BP_Athena_DD_Machine_Trigger.BP_Athena_DD_Machine_Trigger_C.UpdatePercentPulsesOccurred" }
			},
			20.40,
			nullptr
		},
		FEvent
		{
			nullptr,
			L"/Radish/Gameplay/BP_Radish_Special_EventScript.BP_Radish_Special_EventScript_C",
			{
				FEventFunction{ false, L"/Script/SpecialEventGameplayRuntime.SpecialEventScript.StartEventAtIndex" }
			},
			22.40,
			nullptr,
			L"/Radish/Playlist/Playlist_Radish.Playlist_Radish"

		},
		FEvent
		{
			nullptr,
			L"/Durian/Gameplay/BP_Durian_SpecialEventScript.BP_Durian_SpecialEventScript_C",
			{
				FEventFunction{ false, L"/Script/SpecialEventGameplayRuntime.SpecialEventScript.StartEventAtIndex" }
			},
			27.11,
			nullptr,
			L"/DurianPlaylist/Playlist/Playlist_Durian.Playlist_Durian"
		}
	};

#else

	static inline std::vector<FEvent> EventsArray =
	{};
#endif
    static void StartEvent();

	InitHooks;
};