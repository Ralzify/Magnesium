#pragma once

#include "../../../SDK/Engine.h"
#include <atomic>

struct FConfiguration
{
    static inline bool IsS27()
    {
        return std::floor(VersionInfo.FortniteVersion) == 27;
    }

    static inline std::atomic_bool bSnowMap{ false };

    static inline std::atomic_bool bReadyToStart{ false };
    static inline std::atomic<float> MaxTickRate{ 30.f };
    static inline std::atomic_int Port{ 7777 };
    static inline std::atomic_bool bAutoHost{ false };
    static inline constexpr int DefaultAutoHostDelaySeconds = 30;
    static inline std::atomic_int AutoHostDelaySeconds{
        DefaultAutoHostDelaySeconds
    };
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

    static inline std::atomic_bool bIsCustomMap{ false };
    static inline std::atomic_bool AutoEndGame{ false };
    static inline auto CustomMap = L"";

    static inline std::atomic_bool bAutoStartEvent{ false };
    static inline std::atomic<float> EventStartTime{ 120.f };
    static inline std::atomic<float> EventStartBaseTime{ 0.f };
    static inline std::atomic_bool bEventStarted{ false };

    static inline std::atomic_bool bAutoDump{ true };

    static inline std::string BotName = "Magnesium Bot ";
    static inline std::atomic_bool UseCustomBotNames{ false };
    static inline std::atomic_int BotHealth{ 21 };
    static inline std::atomic_int BotShield{ 21 };
    static inline std::atomic_bool bHasPickaxe{ true };

    static inline std::atomic_bool bLateGame{ true };
    static inline std::atomic_int LateGameZone{ 4 }; // default starting zone; Season 27 selects its version-specific phase in the GUI
    static inline std::atomic_bool bLateGameLongZone{ false }; // zone doesnt close for a long time

    static inline std::atomic_bool bCustomSafeZone{ false };
    static inline auto CustomSafeZoneCenter = FVector(0.f, 0.f, 0.f);
    static inline std::atomic<float> CustomSafeZoneRadius{ 100000.f };

    static inline std::atomic_bool bGliderRedeploy{ false };
    static inline std::atomic_bool bEnableCheats{ true };
	static inline std::atomic_bool bMovingBus{ true };
	static inline std::atomic<float> BusStartDelay{ 90.f };

    static inline std::atomic_bool HasCustomRespawnPoint{ false };
	static inline auto CustomRespawnPoint = FVector(0.f, 0.f, 0.f);

    static inline std::atomic_bool bRideableProjectiles{ false };

    static inline std::atomic_bool bSiphon{ false };
    static inline std::atomic_int SiphonAnimType{ 0 };
    static inline std::atomic_int SiphonAmount{ 50 };
    // Vehicle -> player knockback. Fortnite launches a pawn the vehicle drives
    // into, scaled by how fast the vehicle is going. Magnesium teleports
    // vehicles from the driver's ServerMove instead of simulating them, so the
    // native contact that triggers that launch never happens server side -
    // FortVehicleBump reproduces it from the replicated linear velocity.
    static inline std::atomic_bool bVehicleBumpLaunch{ true };
    // Below this the vehicle just nudges past you, as in the retail game.
    static inline std::atomic<float> VehicleBumpMinSpeedKmh{ 20.f };
    // Compensates for how much of the launch survives the trip to the client.
    // The launch velocity itself is computed from the game's own tuning, but a
    // server-side LaunchCharacter reaches the client as a position correction
    // rather than a real impulse, and most of the arc is lost on the way. 5.0 is
    // what measured closest to retail distances in game.
    static inline std::atomic<float> VehicleBumpForceMultiplier{ 5.f };
    // Run-over damage, scaled by impact speed. Only ever applied when the
    // vehicle has no driver or its driver is on another team - shoving your own
    // squadmate is free.
    static inline std::atomic_bool bVehicleBumpDamage{ true };

    static inline std::atomic_bool bEnableTrickshotTab{ false };
    static inline std::atomic_bool bUseWinLines{ true };
    static inline std::atomic_bool RandomizeArenaPoints{ false };
    static inline std::atomic_bool RandomizeKills{ false };
    static inline std::atomic_bool RandomizeLevels{ false };
    static inline std::atomic_bool bEnableDBNO{ true };
    static inline std::atomic_bool bInfiniteRender{ false };
    static inline std::atomic_bool bFModCannon{ false };
	static inline std::atomic<float> CannonLaunchMultiplier{ 1.f };
    static inline std::atomic_bool bCrownSlomo{ true };
    static inline std::atomic_bool bDisableJumpFatigue{ false };
    static inline std::atomic_bool bCancelVelocityOnWin{ false };
    static inline std::atomic_bool bDisableSupplyDrops{ false };
    static inline std::atomic_bool bAutoPauseTODM{ false };
    static inline std::atomic<float> TODMTime{ 7.f };

    static inline std::atomic_bool bInfiniteMats{ true };
    static inline std::atomic_bool bInfiniteAmmo{ true };

    static inline void SetLateGameEnabled(bool bEnabled)
    {
        bLateGame = bEnabled;
    }

    static inline std::atomic_bool bUseVersionizedLoadout{ true };
    static inline std::atomic_bool bUseCustomLoadout{ false };
    static inline FString Primary;
    static inline FString Secondary;
    static inline FString Tertiary;
    static inline FString Quaternary;
    static inline FString Quinary;
    static inline FString Traps;
    static inline std::atomic_int PrimaryAmount{ 1 };
    static inline std::atomic_int SecondaryAmount{ 1 };
    static inline std::atomic_int TertiaryAmount{ 1 };
    static inline std::atomic_int QuaternaryAmount{ 1 };
    static inline std::atomic_int QuinaryAmount{ 1 };
    static inline std::atomic_int TrapsAmount{ 6 };

    static inline std::atomic_bool bForceRespawns{ false }; // build your client with this too!
    static inline std::atomic_bool bKeepInventory{ false };
	static inline std::atomic_bool bMidZoneRespawning{ false };
    static inline std::atomic_bool bJoinInProgress{ false };
	static inline std::atomic_bool PermanentRespawn{ false };
    static inline std::atomic_int RespawnTime{ 3 };
    static inline std::atomic_int RespawnHeight{ 20000 };

    static inline std::atomic_bool bAutoBusStart{ true };
    // Special playlists may keep their authored bus schedule until the user
    // explicitly changes a bus control. This flag is session-only and makes
    // that explicit choice authoritative without persisting it across starts.
    static inline std::atomic_bool bBusSettingsUserOverride{ false };
    // Written by the GUI and consumed by the authoritative game-thread
    // policy tick. This keeps Start Bus Early off the render thread.
    static inline std::atomic_bool bStartBusRequested{ false };

    static inline std::atomic_bool bAutoRestart{ false };

    static inline std::atomic_bool bEnableIris{ true };

    static inline constexpr auto WebhookURL = "";
};
