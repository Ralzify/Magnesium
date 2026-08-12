#pragma once

#include "../../../SDK/Engine.h"
#include <atomic>

struct FConfiguration
{
    static inline bool IsS27()
    {
        return std::floor(VersionInfo.FortniteVersion) == 27;
    }

    static inline bool IsGliderRedeploySupportedBuild()
    {
        return VersionInfo.FortniteVersion > 5.41 &&
            VersionInfo.FortniteVersion <= 16.00;
    }

    static inline constexpr int FoodFightObjectiveHealthAuthored = -1;
    static inline constexpr int FoodFightObjectiveHealthMinimum = 1000;

    static inline int GetFoodFightObjectiveHealthMaximum()
    {
        // Food Fight launched with 200,000 health in 6.30. The later
        // Chapter 1 variants, including both 10.40 modes, use 100,000.
        return VersionInfo.FortniteVersion <= 6.301
            ? 200000
            : 100000;
    }

    static inline std::atomic_bool bSnowMap{ false };

    // Calendar tab. The snow value is version shaped - a position along the
    // season's snow curve on 7.x/11.x/15.10, a progression phase index on
    // 19.01/28.01 - so Calendar::GetSnowVersionModel owns its range.
    static inline std::atomic_bool bSnowOnMatchStart{ false };
    static inline std::atomic<float> SnowValue{ 0.f };

    static inline std::atomic_bool bReadyToStart{ false };
    static inline constexpr float LegacyMaxTickRate = 30.f;
    static inline constexpr float ModernMaxTickRate = 30.f;
    // The historical uncapped loop let a local client's control traffic and
    // acknowledgements flush more often than actor replication. Keep that
    // low-latency behavior separate from the user-facing server tick rate.
    static inline constexpr float LoopbackFlushTickRate = 120.f;
    static inline constexpr float MinimumMaxTickRate = 5.f;
    static inline constexpr float MaximumMaxTickRate = 180.f;
    // VersionInfo is populated by SDK::Init, after static initialization, so
    // AutoHosting::Initialize publishes the version-aware default before the
    // GUI or server start gate can observe this fallback value.
    static inline std::atomic<float> MaxTickRate{ LegacyMaxTickRate };
    static inline std::atomic_bool bMaxTickRateUserOverride{ false };

    static inline float GetDefaultMaxTickRate()
    {
        return VersionInfo.FortniteVersion >= 20.00
            ? ModernMaxTickRate
            : LegacyMaxTickRate;
    }

    static inline float ClampMaxTickRate(float Value)
    {
        if (Value < MinimumMaxTickRate)
            return MinimumMaxTickRate;
        if (Value > MaximumMaxTickRate)
            return MaximumMaxTickRate;
        return Value;
    }

    static inline float GetClampedMaxTickRate()
    {
        return ClampMaxTickRate(
            MaxTickRate.load(std::memory_order_acquire));
    }
    static inline std::atomic_int Port{ 7777 };
    static inline std::atomic_bool bAutoHost{ false };
    static inline std::atomic_bool bSaveAutoHostSettings{ false };
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
    // -1 leaves the playlist/asset-authored maximum completely untouched.
    static inline std::atomic_int FoodFightObjectiveHealth{
        FoodFightObjectiveHealthAuthored
    };
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
    static inline std::atomic_bool bVehicleBumpLaunch{ false };
    // Below this the vehicle just nudges past you, as in the retail game.
    static inline std::atomic<float> VehicleBumpMinSpeedKmh{ 10.f };
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
    // Command-spawned actors are registered for preset persistence only when
    // both the Trickshot tab and this user-facing switch are enabled.
    static inline std::atomic_bool bSaveAndTrackSpawnedObjects{ true };
    // Waypoints can be shared independently of tracked scene geometry.
    static inline std::atomic_bool bSaveWaypoints{ true };
    static inline std::atomic_bool bUseWinLines{ false };
    static inline std::atomic_bool RandomizeArenaPoints{ false };
    static inline std::atomic_bool bPlayerMapIcons{ false };
    static inline std::atomic_bool bAutoReloadOnWaypointTP{ false };
    // Auto god mode hands out the "god" command's protection the moment a
    // player leaves the bus, so a trickshot run never has to be typed for.
    // Maximum pins the health floor to max health - nothing lands at all;
    // Minimum keeps damage live and floors health at 1 HP instead.
    enum class EAutoGodMode : int
    {
        Maximum = 0,
        Minimum = 1,
    };
    static inline std::atomic_bool bAutoGodMode{ false };
    static inline std::atomic_int AutoGodModeType{
        (int)EAutoGodMode::Maximum
    };
    // A trickshot lobby fills with the shooters first and the target last, so
    // the newest human is the one player who has to stay killable. Offered
    // alongside Infinite Render, and only takes effect while that is on, since
    // that pairing is what a trickshot lobby actually runs.
    static inline std::atomic_bool bAutoGodModeExcludeLastPlayer{ false };
    static inline std::atomic_bool RandomizeKills{ false };
    static inline std::atomic_bool RandomizeLevels{ false };
    static inline std::atomic_bool bEnableDBNO{ true };
    static inline std::atomic_bool bInfiniteRender{ false };
    static inline std::atomic_bool bFModCannon{ false };
    // How a cannon throws the player it fires. Native OnLaunchPawn plays the
    // whole thing - the launch animation and the arc the cannon was aimed
    // along - and is what retail does. Off, the pawn is launched directly
    // instead, which skips the animation entirely and makes the distance a
    // number rather than whatever the cannon felt like doing; the per-axis
    // multipliers below scale that launch and only apply in that mode.
    static inline std::atomic_bool bCannonLaunchAnimations{ true };
    static inline std::atomic<float> CannonLaunchXMultiplier{ 1.f };
    static inline std::atomic<float> CannonLaunchYMultiplier{ 1.f };
    static inline std::atomic<float> CannonLaunchZMultiplier{ 1.f };
    static inline std::atomic_bool bCrownSlomo{ false };
    static inline std::atomic_bool bDisableJumpFatigue{ false };
    static inline std::atomic_bool bCancelVelocityOnWin{ false };
    static inline std::atomic_bool bDisableSupplyDrops{ false };
    static inline std::atomic_bool bAutoPauseTODM{ false };
    static inline std::atomic<float> TODMTime{ 7.f };

    static inline void ResetTrickshotSettings()
    {
        bUseWinLines.store(false, std::memory_order_release);
        RandomizeArenaPoints.store(false, std::memory_order_release);
        bPlayerMapIcons.store(false, std::memory_order_release);
        bAutoReloadOnWaypointTP.store(false, std::memory_order_release);
        bAutoGodMode.store(false, std::memory_order_release);
        AutoGodModeType.store(
            (int)EAutoGodMode::Maximum,
            std::memory_order_release);
        bAutoGodModeExcludeLastPlayer.store(
            false, std::memory_order_release);
        RandomizeKills.store(false, std::memory_order_release);
        RandomizeLevels.store(false, std::memory_order_release);
        bInfiniteRender.store(false, std::memory_order_release);
        bRideableProjectiles.store(false, std::memory_order_release);
        bFModCannon.store(false, std::memory_order_release);
        bCannonLaunchAnimations.store(true, std::memory_order_release);
        CannonLaunchXMultiplier.store(1.f, std::memory_order_release);
        CannonLaunchYMultiplier.store(1.f, std::memory_order_release);
        CannonLaunchZMultiplier.store(1.f, std::memory_order_release);
        bCrownSlomo.store(false, std::memory_order_release);
        bDisableJumpFatigue.store(false, std::memory_order_release);
        bCancelVelocityOnWin.store(false, std::memory_order_release);
        bDisableSupplyDrops.store(false, std::memory_order_release);
        bAutoPauseTODM.store(false, std::memory_order_release);
        TODMTime.store(7.f, std::memory_order_release);
        bVehicleBumpLaunch.store(false, std::memory_order_release);
        VehicleBumpMinSpeedKmh.store(10.f, std::memory_order_release);
        VehicleBumpForceMultiplier.store(5.f, std::memory_order_release);
        bVehicleBumpDamage.store(true, std::memory_order_release);
    }

    static inline void SetTrickshotTabEnabled(bool bEnabled)
    {
        if (!bEnabled)
            ResetTrickshotSettings();

        bEnableTrickshotTab.store(
            bEnabled, std::memory_order_release);
    }

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
