#pragma once

struct FConfiguration
{
    static inline bool IsS27()
    {
        return std::floor(VersionInfo.FortniteVersion) == 27;
    }

    static inline auto bReadyToStart = false;
    static inline auto MaxTickRate = 30.f;
    static inline auto Port = 7777;
    static inline constexpr auto bGUI = true;
    static inline constexpr auto bCustomCrashReporter = true;
    static inline constexpr auto bUseStdoutLog = true;

    // wip
    static inline std::string ElimStatusMessage = "";
    static inline std::string ElimKillerName = "";
    static inline std::string ElimEliminatedName = "";
    static inline std::string ElimDistance = "";
    static inline std::string ElimWeaponName = "";

    static inline auto Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";

    static inline auto CreativePlot = L"/Game/Playgrounds/Items/Plots/Temperate_Medium.Temperate_Medium";

	static inline auto CustomMap = L"";
    static inline auto bIsCustomMap = false;
    static inline auto AutoEndGame = false;

    static inline std::string BotName = "Magnesium Bot ";
    static inline auto UseCustomBotNames = false;
    static inline auto BotHealth = 21;
    static inline auto BotShield = 21;

    static inline auto bLateGame = true;
    static inline auto LateGameZone = 4; // starting zone
    static inline auto bLateGameLongZone = false; // zone doesnt close for a long time
    static inline auto bEnableCheats = true;
	static inline auto bMovingBus = true;
	static inline auto BusStartDelay = 60.f;

    static inline auto HasCustomRespawnPoint = false;
	static inline auto CustomRespawnPoint = FVector(0.f, 0.f, 0.f);

    static inline auto bSiphon = false;
    static inline auto SiphonAmount = 50;

    static inline auto bEnableTrickshotTab = false;
    static inline auto bInfiniteRender = false;
    static inline auto bFModCannon = false;
	static inline auto CannonLaunchMultiplier = 1.f;

    static inline auto bInfiniteMats = true;
    static inline auto bInfiniteAmmo = true;

    static inline auto bForceRespawns = false; // build your client with this too!
    static inline auto bKeepInventory = false;
    static inline auto bJoinInProgress = false;
	static inline auto PermanentRespawn = false;

    static inline auto bAutoBusStart = true;

    static inline auto bAutoRestart = false;

    static inline constexpr auto bEnableIris = true; 

    static inline constexpr auto WebhookURL = "";
};
