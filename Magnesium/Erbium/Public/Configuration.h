#pragma once

#include "../../../SDK/Engine.h"
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

struct FCustomSafeZoneNode
{
    FVector Center{ 0.f, 0.f, 0.f };
    bool bHasNormalizedCenter{ false };
    float NormalizedU{ 0.5f };
    float NormalizedV{ 0.5f };
    float RadiusCm{ 100000.f };
    std::optional<float> HoldBeforeNextSeconds;
    std::optional<float> MoveToNextSeconds;

    FCustomSafeZoneNode() = default;
    FCustomSafeZoneNode(const FCustomSafeZoneNode&) = default;

    // SDK::FVector's assignment operator takes a non-const source, so copy the components explicitly.
    FCustomSafeZoneNode& operator=(const FCustomSafeZoneNode& Other)
    {
        if (this == &Other)
            return *this;

        Center.X = Other.Center.X;
        Center.Y = Other.Center.Y;
        Center.Z = Other.Center.Z;
        bHasNormalizedCenter = Other.bHasNormalizedCenter;
        NormalizedU = Other.NormalizedU;
        NormalizedV = Other.NormalizedV;
        RadiusCm = Other.RadiusCm;
        HoldBeforeNextSeconds = Other.HoldBeforeNextSeconds;
        MoveToNextSeconds = Other.MoveToNextSeconds;
        return *this;
    }
};

struct FCustomSafeZoneSequence
{
    static inline constexpr int SchemaVersion = 1;
    static inline constexpr size_t MinimumNodeCount = 1;
    static inline constexpr size_t MaximumNodeCount = 32;
    static inline constexpr float MinimumRadiusCm = 500.f;
    static inline constexpr float MaximumRadiusCm = 100000.f;
    static inline constexpr float MinimumDurationSeconds = 0.f;
    static inline constexpr float MaximumDurationSeconds = 3600.f;

    bool bMovingZoneEnabled{ false };
    bool bCloseFinalCircle{ false };
    std::vector<FCustomSafeZoneNode> Nodes{ FCustomSafeZoneNode{} };

    std::optional<size_t> FindFirstRadiusIncreaseEdge() const
    {
        for (size_t Index = 1; Index < Nodes.size(); ++Index)
        {
            if (Nodes[Index].RadiusCm > Nodes[Index - 1].RadiusCm)
                return Index - 1;
        }
        return std::nullopt;
    }
};

struct FConfiguration
{
    static inline bool IsS27()
    {
        return std::floor(VersionInfo.FortniteVersion) == 27;
    }

    static inline bool IsGliderRedeploySupportedBuild()
    {
        return VersionInfo.FortniteVersion > 5.41;
    }

    static inline constexpr float
        GliderRedeployFallbackHeightLimit = 1000.f;
    static inline constexpr float
        GliderRedeployFallbackLateralVelocityMult = 1.f;

    static inline constexpr int FoodFightObjectiveHealthAuthored = -1;
    static inline constexpr int FoodFightObjectiveHealthMinimum = 1000;

    static inline int GetFoodFightObjectiveHealthMaximum()
    {
        // Food Fight launched with 200,000 health in 6.30; later Chapter 1 variants use 100,000.
        return VersionInfo.FortniteVersion <= 6.301 ? 200000 : 100000;
    }

    static inline std::atomic_bool bSnowMap{ false };

    static inline std::atomic_bool bSnowOnMatchStart{ false };
    static inline std::atomic<float> SnowValue{ 0.f };

    static inline std::atomic_bool bReadyToStart{ false };
    static inline constexpr float LegacyMaxTickRate = 30.f;
    static inline constexpr float ModernMaxTickRate = 30.f;
    static inline constexpr float MinimumMaxTickRate = 5.f;
    static inline constexpr float MaximumMaxTickRate = 180.f;
    static inline std::atomic<float> MaxTickRate{ LegacyMaxTickRate };
    static inline std::atomic_bool bMaxTickRateUserOverride{ false };

    static inline float GetDefaultMaxTickRate()
    {
        return VersionInfo.FortniteVersion >= 20.00 ? ModernMaxTickRate : LegacyMaxTickRate;
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
        return ClampMaxTickRate(MaxTickRate.load(std::memory_order_acquire));
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

    static inline std::atomic_bool bAutoDump{ false };

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
    static inline std::atomic_bool bCustomMovingZone{ false };

private:
    struct FCustomSafeZonePublicationState
    {
        SRWLOCK Lock = SRWLOCK_INIT;
        std::shared_ptr<const FCustomSafeZoneSequence> Snapshot{
            std::make_shared<const FCustomSafeZoneSequence>()
        };
        bool bFrozenForMatch{ false };
    };

    class FCustomSafeZoneExclusiveLock
    {
    public:
        explicit FCustomSafeZoneExclusiveLock(SRWLOCK& InLock) noexcept : Lock(&InLock)
        {
            AcquireSRWLockExclusive(Lock);
        }

        ~FCustomSafeZoneExclusiveLock() noexcept
        {
            ReleaseSRWLockExclusive(Lock);
        }

        FCustomSafeZoneExclusiveLock(const FCustomSafeZoneExclusiveLock&) = delete;
        FCustomSafeZoneExclusiveLock& operator=(const FCustomSafeZoneExclusiveLock&) = delete;

    private:
        SRWLOCK* Lock;
    };

    class FCustomSafeZoneSharedLock
    {
    public:
        explicit FCustomSafeZoneSharedLock(SRWLOCK& InLock) noexcept : Lock(&InLock)
        {
            AcquireSRWLockShared(Lock);
        }

        ~FCustomSafeZoneSharedLock() noexcept
        {
            ReleaseSRWLockShared(Lock);
        }

        FCustomSafeZoneSharedLock(const FCustomSafeZoneSharedLock&) = delete;
        FCustomSafeZoneSharedLock& operator=(const FCustomSafeZoneSharedLock&) = delete;

    private:
        SRWLOCK* Lock;
    };

    static inline FCustomSafeZonePublicationState& GetCustomSafeZonePublicationState()
    {
        static auto* const State = new FCustomSafeZonePublicationState();
        return *State;
    }

    static inline float ClampCustomSafeZoneValue(float Value, float Minimum, float Maximum)
    {
        return Value < Minimum ? Minimum : (Value > Maximum ? Maximum : Value);
    }

    static inline bool ValidateAndClampCustomSafeZoneSequence(FCustomSafeZoneSequence& Sequence)
    {
        if (Sequence.Nodes.size() <FCustomSafeZoneSequence::MinimumNodeCount ||
            Sequence.Nodes.size() > FCustomSafeZoneSequence::MaximumNodeCount)
        {
            return false;
        }

        for (auto& Node : Sequence.Nodes)
        {
            if (!std::isfinite(Node.Center.X) || !std::isfinite(Node.Center.Y) ||
                !std::isfinite(Node.Center.Z) || !std::isfinite(Node.NormalizedU) ||
                !std::isfinite(Node.NormalizedV) || !std::isfinite(Node.RadiusCm))
            {
                return false;
            }

            Node.NormalizedU = ClampCustomSafeZoneValue(Node.NormalizedU, 0.f, 1.f);
            Node.NormalizedV = ClampCustomSafeZoneValue(Node.NormalizedV, 0.f, 1.f);
            Node.RadiusCm = ClampCustomSafeZoneValue(Node.RadiusCm,
                FCustomSafeZoneSequence::MinimumRadiusCm, FCustomSafeZoneSequence::MaximumRadiusCm);

            const auto ClampDuration = [](std::optional<float>& Duration)
            {
                if (!Duration.has_value())
                    return true;
                if (!std::isfinite(*Duration) || *Duration <FCustomSafeZoneSequence::
                            MinimumDurationSeconds || *Duration > FCustomSafeZoneSequence::
                            MaximumDurationSeconds)
                {
                    return false;
                }
                return true;
            };

            if (!ClampDuration(Node.HoldBeforeNextSeconds) ||
                !ClampDuration(Node.MoveToNextSeconds))
            {
                return false;
            }
        }

        if (Sequence.FindFirstRadiusIncreaseEdge().has_value())
            return false;

        return true;
    }

public:
    static inline bool SetCustomSafeZoneEnabled(bool bEnabled)
    {
        auto& State = GetCustomSafeZonePublicationState();
        FCustomSafeZoneExclusiveLock Guard(State.Lock);
        if (State.bFrozenForMatch)
            return false;

        bCustomSafeZone.store(bEnabled, std::memory_order_release);
        return true;
    }

    static inline bool PublishCustomSafeZoneSequence(FCustomSafeZoneSequence Sequence,
        bool bEnableMovingZone)
    {
        auto& State = GetCustomSafeZonePublicationState();
        FCustomSafeZoneExclusiveLock Guard(State.Lock);
        if (State.bFrozenForMatch)
            return false;
        if (!ValidateAndClampCustomSafeZoneSequence(Sequence))
            return false;

        Sequence.bMovingZoneEnabled = bEnableMovingZone;
        auto Published = std::make_shared<const FCustomSafeZoneSequence>(std::move(Sequence));
        const auto& LegacyNode = Published->Nodes.front();

        State.Snapshot = Published;

        CustomSafeZoneCenter.X = LegacyNode.Center.X;
        CustomSafeZoneCenter.Y = LegacyNode.Center.Y;
        CustomSafeZoneCenter.Z = LegacyNode.Center.Z;
        CustomSafeZoneRadius.store(LegacyNode.RadiusCm, std::memory_order_release);
        bCustomMovingZone.store(bEnableMovingZone, std::memory_order_release);

        return true;
    }

    static inline bool PublishCustomSafeZoneSequenceIfCurrent(
        std::shared_ptr<const FCustomSafeZoneSequence> Expected, FCustomSafeZoneSequence Sequence,
        bool bEnableMovingZone)
    {
        auto& State = GetCustomSafeZonePublicationState();
        FCustomSafeZoneExclusiveLock Guard(State.Lock);
        if (State.bFrozenForMatch)
            return false;
        if (!Expected || !ValidateAndClampCustomSafeZoneSequence(Sequence))
        {
            return false;
        }

        Sequence.bMovingZoneEnabled = bEnableMovingZone;
        auto Published = std::make_shared<const FCustomSafeZoneSequence>(std::move(Sequence));
        if (State.Snapshot != Expected)
            return false;
        State.Snapshot = Published;

        const auto& LegacyNode = Published->Nodes.front();
        CustomSafeZoneCenter.X = LegacyNode.Center.X;
        CustomSafeZoneCenter.Y = LegacyNode.Center.Y;
        CustomSafeZoneCenter.Z = LegacyNode.Center.Z;
        CustomSafeZoneRadius.store(LegacyNode.RadiusCm, std::memory_order_release);
        bCustomMovingZone.store(bEnableMovingZone, std::memory_order_release);
        return true;
    }

    static inline bool FreezeCustomSafeZoneSequenceForMatch(
        const std::shared_ptr<const FCustomSafeZoneSequence>& Expected)
    {
        if (!Expected)
            return false;

        auto& State = GetCustomSafeZonePublicationState();
        FCustomSafeZoneExclusiveLock Guard(State.Lock);
        if (State.bFrozenForMatch || State.Snapshot != Expected ||
            !bCustomSafeZone.load(std::memory_order_acquire) || !Expected->bMovingZoneEnabled)
        {
            return false;
        }

        State.bFrozenForMatch = true;
        return true;
    }

    static inline void ReleaseCustomSafeZoneSequenceForMatch()
    {
        auto& State = GetCustomSafeZonePublicationState();
        FCustomSafeZoneExclusiveLock Guard(State.Lock);
        State.bFrozenForMatch = false;
    }

    static inline std::shared_ptr<const FCustomSafeZoneSequence> GetCustomSafeZoneSequenceSnapshot()
    {
        auto& State = GetCustomSafeZonePublicationState();
        FCustomSafeZoneSharedLock Guard(State.Lock);
        return State.Snapshot;
    }

    static inline bool IsCustomMovingZoneSnapshotEnabled()
    {
        const auto Snapshot = GetCustomSafeZoneSequenceSnapshot();
        return Snapshot && Snapshot->bMovingZoneEnabled;
    }

    static inline FCustomSafeZoneNode GetLegacyCustomSafeZoneNodeSnapshot()
    {
        const auto Snapshot = GetCustomSafeZoneSequenceSnapshot();
        return Snapshot && !Snapshot->Nodes.empty() ? Snapshot->Nodes.front()
            : FCustomSafeZoneNode{};
    }

    static inline bool PublishLegacyCustomSafeZone(const FCustomSafeZoneNode& LegacyNode)
    {
        FCustomSafeZoneSequence Sequence;
        Sequence.Nodes.assign(1, LegacyNode);
        return PublishCustomSafeZoneSequence(std::move(Sequence), false);
    }

    static inline std::atomic_bool bGliderRedeploy{ false };
    static inline std::atomic_int GliderRedeployRuntimeSupport{ -1 };
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
    static inline std::atomic_bool bVehicleBumpLaunch{ false };
    static inline std::atomic<float> VehicleBumpMinSpeedKmh{ 10.f };
    // A server-side LaunchCharacter arrives as a position correction, so most of the arc is lost. 5.0 measured closest to retail.
    static inline std::atomic<float> VehicleBumpForceMultiplier{ 5.f };
    static inline std::atomic_bool bVehicleBumpDamage{ true };

    static inline std::atomic_bool bEnableTrickshotTab{ false };
    static inline std::atomic_bool bSaveAndTrackSpawnedObjects{ true };
    static inline std::atomic_bool bSaveWaypoints{ true };
    static inline std::atomic_bool bUseWinLines{ false };
    static inline std::atomic_bool RandomizeArenaPoints{ false };
    static inline std::atomic_bool bPlayerMapIcons{ false };
    static inline std::atomic_bool bAutoReloadOnWaypointTP{ false };
    static inline std::atomic_bool bRemoveIceOnWaypointTP{ false };
    enum class EAutoGodMode : int
    {
        Maximum = 0, Minimum = 1,
    };
    static inline std::atomic_bool bAutoGodMode{ false };
    static inline std::atomic_int AutoGodModeType{
        (int)EAutoGodMode::Maximum
    };
    static inline std::atomic_bool bAutoGodModeExcludeLastPlayer{ false };
    static inline std::atomic_bool RandomizeKills{ false };
    static inline std::atomic_bool RandomizeLevels{ false };
    static inline std::atomic_bool bEnableDBNO{ true };
    static inline std::atomic_bool bInfiniteRender{ false };
    static inline std::atomic_bool bFModCannon{ false };
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
        bRemoveIceOnWaypointTP.store(false, std::memory_order_release);
        bAutoGodMode.store(false, std::memory_order_release);
        AutoGodModeType.store((int)EAutoGodMode::Maximum, std::memory_order_release);
        bAutoGodModeExcludeLastPlayer.store(false, std::memory_order_release);
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

        bEnableTrickshotTab.store(bEnabled, std::memory_order_release);
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
    static inline std::atomic_bool bBusSettingsUserOverride{ false };
    static inline std::atomic_bool bStartBusRequested{ false };

    static inline std::atomic_bool bAutoRestart{ false };

    static inline std::atomic_bool bEnableIris{ true };

    static inline constexpr auto WebhookURL = "";
};
