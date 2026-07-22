#pragma once

#include "../../../SDK/Engine.h"

struct FConfiguration
{
    static inline bool IsS27()
    {
        return std::floor(VersionInfo.FortniteVersion) == 27;
    }

    static inline auto bSnowMap = false;

    static inline auto bReadyToStart = false;
    static inline auto MaxTickRate = 30.f;
    static inline auto Port = 7777;
    static inline constexpr auto bGUI = true;
    static inline auto bUseSplashAnim = true;
    static inline auto bUseDarkMode = false;
    static inline constexpr auto bCustomCrashReporter = true;
    static inline constexpr auto bUseStdoutLog = true;

    // wip
    static inline std::string ElimStatusMessage = "";
    static inline std::string ElimKillerName = "";
    static inline std::string ElimEliminatedName = "";
    static inline std::string ElimDistance = "";
    static inline std::string ElimWeaponName = "";

    static inline auto Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";

    static inline bool IsKnownS27CustomMapPlaylist()
    {
        if (VersionInfo.FortniteVersion != 27.11)
            return false;

        return wcsstr(Playlist, L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn")
            || wcsstr(Playlist, L"/Game/Jett/Playlist_OnlyUp_Jett.Playlist_OnlyUp_Jett")
            || wcsstr(Playlist, L"/Game/Jett/TiltedZW/Playlist_TiltedZW_Jett.Playlist_TiltedZW_Jett");
    }

    static inline auto CreativePlot = L"/Game/Playgrounds/Items/Plots/Temperate_Medium.Temperate_Medium";

    static inline auto bIsCustomMap = false;
    static inline auto AutoEndGame = false;
    static inline auto CustomMap = L"";

    static inline auto bAutoStartEvent = false;
    static inline auto EventStartTime = 120.f;
    static inline auto EventStartBaseTime = 0.f;
    static inline auto bEventStarted = false;

    static inline auto bAutoDump = true;

    static inline std::string BotName = "Magnesium Bot ";
    static inline auto UseCustomBotNames = false;
    static inline auto BotHealth = 21;
    static inline auto BotShield = 21;
    static inline auto bHasPickaxe = true;

    static inline auto bLateGame = true;
    static inline auto LateGameZone = 3; // original Erbium starting zone; GUI selects newer defaults by version
    static inline auto bLateGameLongZone = false; // zone doesnt close for a long time

    static inline auto bCustomSafeZone = false;
    static inline auto CustomSafeZoneCenter = FVector(0.f, 0.f, 0.f);
    static inline auto CustomSafeZoneRadius = 100000.f;

    static inline auto bGliderRedeploy = false;
    static inline auto bEnableCheats = true;
	static inline auto bMovingBus = true;
	static inline auto BusStartDelay = 90.f;

    static inline auto HasCustomRespawnPoint = false;
	static inline auto CustomRespawnPoint = FVector(0.f, 0.f, 0.f);

    static inline auto bRideableProjectiles = false;

    static inline auto bSiphon = false;
    static inline auto SiphonAnimType = 0;
    static inline auto SiphonAmount = 50;

    static inline auto bEnableTrickshotTab = false;
    static inline auto bUseWinLines = true;
    static inline auto RandomizeArenaPoints = false;
    static inline auto RandomizeKills = false;
    static inline auto RandomizeLevels = false;
    static inline auto bEnableDBNO = true;
    static inline auto bInfiniteRender = false;
    static inline auto bFModCannon = false;
	static inline auto CannonLaunchMultiplier = 1.f;
    static inline auto bCrownSlomo = true;
    static inline auto bDisableJumpFatigue = false;
    static inline auto bCancelVelocityOnWin = false;
    static inline auto bAutoPauseTODM = false;
    static inline auto TODMTime = 7;

    static inline auto bInfiniteMats = true;
    static inline auto bInfiniteAmmo = true;

    static inline auto bUseVersionizedLoadout = true;
    static inline auto bUseCustomLoadout = false;
    static inline FString Primary;
    static inline FString Secondary;
    static inline FString Tertiary;
    static inline FString Quaternary;
    static inline FString Quinary;
    static inline FString Traps;
    static inline auto PrimaryAmount = 1;
    static inline auto SecondaryAmount = 1;
    static inline auto TertiaryAmount = 1;
    static inline auto QuaternaryAmount = 1;
    static inline auto QuinaryAmount = 1;
    static inline auto TrapsAmount = 6;

    static inline auto bForceRespawns = false; // build your client with this too!
    static inline auto bKeepInventory = false;
	static inline auto bMidZoneRespawning = false;
    static inline auto bJoinInProgress = false;
	static inline auto PermanentRespawn = false;
    static inline auto RespawnTime = 3;
    static inline auto RespawnHeight = 20000;

    static inline auto bAutoBusStart = true;

    static inline auto bAutoRestart = false;

    static inline auto bEnableIris = true;

    static inline constexpr auto WebhookURL = "";
};
