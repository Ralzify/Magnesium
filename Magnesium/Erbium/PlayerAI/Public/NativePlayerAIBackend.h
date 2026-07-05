#pragma once
// ============================================================================
// Magnesium PlayerAI - NativePlayerAIBackend
//
// Preferred backend on versions that ship the native player-bot stack
// (Chapter 2 and newer: FortAthenaMutator_Bots + the Phoebe player bot
// pawn/controller). Bots spawned through it get the engine's own movement,
// pathfinding (over a runtime-generated navmesh), replication, sprinting
// and REAL weapon fire - so they walk, sprint, shoot and die exactly like
// the official player bots. On versions without this stack the PlayerAI
// system falls back to the simulated backend automatically.
//
// The navmesh trick: the game ships /Game/Maps/NavMeshBounds, a stub level
// holding a unit-sized NavMeshBoundsVolume meant to be scaled over the map
// so the navmesh generates at runtime. Without nav bounds no tiles are ever
// built and every pathfinding request fails.
//
// This backend never touches henchman/guard/boss/wildlife AI - it only uses
// the player-bot (Phoebe) spawn path.
// ============================================================================
#include "PlayerAITypes.h"
#include "PlayerAIEntity.h"
#include "../../../FortniteGame/Public/FortGameMode.h"

// Native player-bot controller (own reflection caches - never mixed with
// AFortPlayerControllerAthena property caches).
class AFortAthenaAIBotController : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaAIBotController);

    DEFINE_PROP(Inventory, AFortInventory*);
    DEFINE_PROP(PathFollowingComponent, UActorComponent*);
    DEFINE_PROP(Pawn, AFortPlayerPawnAthena*);
    DEFINE_PROP(PlayerState, AFortPlayerStateAthena*);

    DEFINE_FUNC(MoveToLocation, uint8);
    DEFINE_FUNC(MoveToActor, uint8);
    DEFINE_FUNC(GetMoveStatus, uint8);
    DEFINE_FUNC(StopMovement, void);
    DEFINE_FUNC(K2_SetFocalPoint, void);
    DEFINE_FUNC(K2_ClearFocus, void);
    DEFINE_FUNC(LineOfSightTo, bool);
    DEFINE_FUNC(SetControlRotation, void);
};

class PlayerAIController;

class NativePlayerAIBackend
{
public:
    // True when this version ships the native player-bot stack (cached).
    static bool IsAvailable();

    // One-time setup: spawns the bot mutator, wires the server bot manager
    // and kicks off runtime navmesh generation. Safe to call every tick
    // until it reports ready.
    static void EnsureInitialized();
    static bool IsNavMeshReady();

    // Spawns one native PlayerAI at the transform. Returns nullptr on
    // failure (logged, caller falls back or retries).
    static PlayerAIEntity SpawnNativeEntity(const FTransform& SpawnAt, const std::string& DisplayName);

    // ---- Movement / action primitives (native pathfinding) ----
    // Hybrid move: path follower when it can, direct input fallback where
    // the navmesh has no tiles. Call every tick while moving.
    static void MoveTo(PlayerAIController& AI, const FVector& Dest, float AcceptRadius, bool bSprint);
    static void StopMove(PlayerAIController& AI);
    static void SetFocalPoint(PlayerAIController& AI, const FVector& Point);
    static void ClearFocus(PlayerAIController& AI);
    static void StartFire(PlayerAIController& AI);
    static void StopFire(PlayerAIController& AI);
    static void Jump(PlayerAIController& AI);
    static void Sprint(PlayerAIController& AI);

    // Teleport + native skydive (bus drop / fallback placements).
    static void SkydiveFrom(PlayerAIController& AI, const FVector& Location);

    static AFortAthenaAIBotController* GetBotController(PlayerAIController& AI);

    // Resets cached availability (new match).
    static void Reset();
};
