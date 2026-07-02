#pragma once
// ============================================================================
// Magnesium PlayerAI - PlayerAIManager
//
// Spawns, tracks, updates and removes all PlayerAI players. Assigns names,
// skill levels, landing preferences, aggression and loot preferences.
// Tracks alive/eliminated PlayerAI counts and keeps PlayerAI players
// counted as real match participants for win-condition checks.
//
// Never interacts with the existing bot command system and never modifies
// native henchman / boss / guard / wildlife / NPC AI managers.
// ============================================================================
#include "PlayerAIController.h"
#include "../../../FortniteGame/Public/FortGameMode.h"
#include "../../../FortniteGame/Public/BuildingContainer.h"
#include "../../../FortniteGame/Public/FortInventory.h"
#include <vector>
#include <memory>

// Shared per-tick world view for all AI (refreshed on a slow cadence so ~99
// AI do not rescan the world every frame).
struct FPlayerAIWorldSnapshot
{
    AFortGameMode* GameMode = nullptr;
    AFortGameStateAthena* GameState = nullptr;
    EPlayerAIMatchPhase Phase = EPlayerAIMatchPhase::None;
    float Now = 0.f;

    // Safe zone
    bool bHasSafeZone = false;
    FVector SafeZoneCenter{};
    float SafeZoneRadius = 0.f;

    // Loot actors (refreshed every PlayerAIConfig::WorldSnapshotInterval).
    std::vector<ABuildingContainer*> Containers;
    std::vector<AFortPickupAthena*> Pickups;
    float LastLootScanTime = -1000.f;
};

class PlayerAIManager
{
public:
    // ---- Lifecycle (driven by MagnesiumPlayerAIIntegration) ----
    static void Initialize();
    static void Shutdown(const char* Reason);
    static void OnMatchEnded();

    // Per server tick update for all PlayerAI players.
    static void UpdateAll(float Now, float DeltaSeconds);

    // ---- Spawning / removal ----
    // Registers a freshly spawned entity (see MagnesiumPlayerAISpawner) and
    // assigns name/skill/landing/loot preferences.
    static PlayerAIController* RegisterEntity(const PlayerAIEntity& Entity);

    // Safely removes one not-yet-needed PlayerAI pre-match (to make room
    // for a real player). Returns true when one was removed.
    static bool RemoveOnePreMatch();

    // Removes all PlayerAI players (match end / shutdown).
    static void RemoveAll(const char* Reason);

    // ---- Queries ----
    static int GetTotalCount();
    static int GetAliveCount();
    static int GetEliminatedCount();
    static bool IsPlayerAI(AFortPlayerControllerAthena* PC);
    static PlayerAIController* FindByController(AFortPlayerControllerAthena* PC);

    static FPlayerAIWorldSnapshot& GetWorld() { return World; }
    static std::vector<std::unique_ptr<PlayerAIController>>& GetControllers() { return Controllers; }

    static inline bool bInitialized = false;

private:
    static void RefreshWorldSnapshot(float Now);

    static inline std::vector<std::unique_ptr<PlayerAIController>> Controllers;
    static inline FPlayerAIWorldSnapshot World{};
    static inline int NextAIIndex = 1;
    static inline int EliminatedCount = 0;
    static inline float LastAliveLogTime = 0.f;

    friend class EliminationBehavior;
};
