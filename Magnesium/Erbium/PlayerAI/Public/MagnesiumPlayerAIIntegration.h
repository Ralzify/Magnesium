#pragma once
// ============================================================================
// Magnesium PlayerAI - MagnesiumPlayerAIIntegration
//
// The single integration surface between Magnesium and the PlayerAI system.
// Magnesium calls exactly ONE function from its server tick
// (UNetDriver::TickFlush): MagnesiumPlayerAIIntegration::OnServerTick().
// Everything else - lifecycle hooks, lobby fill, per-AI updates and
// cleanup - is derived internally from the observed gameserver state, so
// no other Magnesium system (bot command, native NPC/henchman/guard/boss/
// wildlife AI, playlists, damage, storm, eliminations, win conditions)
// is modified.
//
// Lifecycle hooks fired internally (in order):
//   OnGameserverConfigLoaded  - gameserver config committed (Start Server)
//   OnGameserverStarted       - server listening / joinable
//   OnMatchCreated            - game mode + game state exist
//   OnRealPlayerJoined/Left   - real (non-AI, non-bot-command) players
//   OnPreMatchStarted         - warmup island active
//   OnTransportPhaseStarted   - bus phase started
//   OnMatchEnded              - winner decided / match torn down
//   OnGameserverShutdown      - best-effort cleanup
// ============================================================================
#include "PlayerAITypes.h"

class UNetDriver;

class MagnesiumPlayerAIIntegration
{
public:
    // Sole entry point, called from the Magnesium server tick. Safe to call
    // every tick regardless of the Enable AIs setting (returns immediately
    // when disabled and nothing was ever spawned).
    static void OnServerTick(UNetDriver* Driver, float DeltaSeconds);

    // Best-effort cleanup, safe to call multiple times.
    static void OnGameserverShutdown();

    // True when the controller belongs to a PlayerAI entity (any backend).
    // For engine hooks that must exclude PlayerAI from real-player flows
    // (e.g. aircraft boarding). Cheap; false when the system is idle.
    static bool IsPlayerAIController(void* PlayerController);

    // Called from the OnAircraftExitedDropZone hook BEFORE the native exit
    // processing runs: jumps out / unboards every PlayerAI still connected
    // to the aircraft (the native auto-jump rejects connectionless
    // controllers and kills the AI instead). No-op when idle.
    static void OnAircraftDropZoneEnding();

    // Status string for the configuration UI (Player Bot Tab).
    static const char* GetStatusLine();

private:
    // Real tick body + its fault guard (OnServerTick wraps them so a fault
    // anywhere in the PlayerAI system degrades/disables the system instead
    // of crashing the gameserver).
    static void OnServerTickInternal(UNetDriver* Driver, float DeltaSeconds);
    static bool TryServerTick(UNetDriver* Driver, float DeltaSeconds);

    // Internal lifecycle hooks (see header comment).
    static void OnGameserverConfigLoaded();
    static void OnGameserverStarted();
    static void OnMatchCreated();
    static void OnRealPlayerJoined(int RealPlayerCount);
    static void OnRealPlayerLeft(int RealPlayerCount);
    static void OnPreMatchStarted();
    static void OnTransportPhaseStarted();
    static void OnMatchEnded();

    static int CountRealPlayers(UNetDriver* Driver);
    static void ResetLifecycleState();

    static inline bool bConfigLoadedFired = false;
    static inline bool bServerStartedFired = false;
    static inline bool bMatchCreatedFired = false;
    static inline bool bPreMatchFired = false;
    static inline bool bTransportFired = false;
    static inline bool bMatchEndedFired = false;
    static inline int LastRealPlayerCount = 0;
    static inline float LastStatusTime = 0.f;
    static inline int SystemFaults = 0;
    static inline bool bSystemDisabled = false;
    static inline bool bLifecycleTokensSeen = false;
    static inline const void* LifecycleWorldToken = nullptr;
    static inline const void* LifecycleGameStateToken = nullptr;
    static inline char StatusLine[128] = "PlayerAI: idle";
};
