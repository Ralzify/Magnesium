#pragma once
// ============================================================================
// Magnesium PlayerAI - PlayerAIConfig
//
// Internal defaults for the PlayerAI system. The only user facing setting
// is the "Enable AIs" toggle (see MagnesiumPlayerAISettings.h) - everything
// here is a safe internal default that "just works" across versions.
// If required data is missing at runtime the system logs the issue and
// falls back instead of crashing the gameserver.
// ============================================================================
#include "PlayerAITypes.h"

class PlayerAIConfig
{
public:
    // ---- Lobby fill -------------------------------------------------------
    // Hard safety cap for PlayerAI count, independent of the gameserver max
    // player count (which is still the primary source, see
    // MagnesiumPlayerAIConfig::GetMaxPlayerCount()).
    static inline int AbsoluteMaxPlayerAIs = 99;

    // How many PlayerAI players may be spawned per server tick while the
    // lobby fills (avoids hitches when filling up to ~99 slots).
    static inline int MaxSpawnsPerTick = 2;

    // Delay after the first real player joins before filling starts.
    static inline float FillStartDelaySeconds = 2.0f;

    // Gradual fill: one PlayerAI joins every so often (randomized), like a
    // lobby filling with real players, instead of 99 appearing at once.
    static inline float FillSpawnIntervalMin = 0.30f;
    static inline float FillSpawnIntervalMax = 0.80f;

    // ---- Thinking / updates ----------------------------------------------
    static inline float ThinkIntervalMin = 0.30f;
    static inline float ThinkIntervalMax = 0.60f;
    static inline float WorldSnapshotInterval = 2.0f;   // container/pickup rescans
    static inline float GroundSnapInterval = 0.30f;     // ground traces per AI

    // ---- Pre-match behavior ----------------------------------------------
    static inline float PreMatchWanderRadius = 3500.f;
    static inline float PreMatchIdleTimeMin = 1.5f;
    static inline float PreMatchIdleTimeMax = 5.0f;
    static inline float PreMatchEmoteChance = 0.25f;    // only if emotes supported
    static inline float PreMatchEmoteDuration = 5.0f;
    static inline float PreMatchJumpChance = 0.15f;

    // ---- Transport / landing ----------------------------------------------
    static inline float ThankDriverChance = 0.30f;      // only if supported
    static inline float JumpDistanceMin = 8000.f;       // jump window around target
    static inline float JumpDistanceMax = 30000.f;
    static inline float ForcedJumpTimeAfterPhase = 26.f; // fallback jump timer (drop windows can be short)
    static inline float LandingJitterRadius = 4000.f;

    // ---- Looting -----------------------------------------------------------
    static inline float LootSearchRadius = 12000.f;
    static inline float ContainerInteractRange = 320.f;
    static inline float PickupInteractRange = 220.f;
    static inline float OpenContainerDuration = 1.6f;

    // ---- Storm / rotation --------------------------------------------------
    static inline float ZoneRotateSafetyMargin = 0.80f; // rotate when outside 80% radius
    static inline float StormFallbackDamageInterval = 1.0f;
    // Seconds outside the zone with no observed health loss before the
    // PlayerAI fallback storm damage kicks in (native storm damage may not
    // affect server side AI entities on every version).
    static inline float StormFallbackActivationDelay = 2.5f;

    // ---- Combat ------------------------------------------------------------
    static inline float CombatGiveUpTime = 12.f;   // lose target after this long unseen
    static inline float HealthLowThreshold = 35.f; // consider retreating/healing below
    static inline float HealDuration = 4.0f;
    static inline float HealAmount = 50.f;
    static inline float ShieldPotionAmount = 50.f;
    static inline float ReloadDuration = 2.4f;

    // Simulated per-hit damage defaults by weapon role (used when exact
    // version specific weapon stats are unavailable).
    static inline float DamageCloseRange = 63.f;
    static inline float DamageMediumRange = 26.f;
    static inline float DamageLongRange = 78.f;
    static inline float DamageFallback = 20.f;

    // Simulated seconds between shots by weapon role.
    static inline float FireIntervalCloseRange = 0.95f;
    static inline float FireIntervalMediumRange = 0.28f;
    static inline float FireIntervalLongRange = 1.60f;

    static inline int MagazineSizeDefault = 24;

    // ---- Stuck detection ----------------------------------------------------
    static inline float StuckCheckInterval = 1.5f;
    static inline float StuckDistanceThreshold = 60.f; // moved less than this = stuck
    static inline int StuckRetriesBeforeNewTarget = 2;
    static inline int StuckRetriesBeforeTeleport = 4;  // teleport is the last resort
};
