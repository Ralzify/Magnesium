#pragma once
// ============================================================================
// Magnesium PlayerAI - NavigationBehavior
//
// Version independent navigation. No single version specific nav system is
// relied on: input-driven walking where the engine simulates it, otherwise
// engine-swept walking (real capsule collision + swept gravity), with stuck
// detection and escalating recovery (repath -> new target -> teleport).
//
// TODO: connect this to the Magnesium map/POI data system if a richer nav
//       source (nav mesh / waypoint graph) is available for a version.
// ============================================================================
#include "PlayerAIController.h"

class NavigationBehavior
{
public:
    // Steers the pawn toward AI.MoveTarget. Called every tick while a move
    // target is set. Handles ground snapping, facing and animation-relevant
    // velocity replication. Returns true when the target has been reached.
    static bool StepMovement(PlayerAIController& AI, float Now, float DeltaSeconds, float SpeedOverride = -1.f);

    // Sets a new move target (ground snapped when possible).
    static void SetMoveTarget(PlayerAIController& AI, const FVector& Target);
    static void ClearMoveTarget(PlayerAIController& AI);

    // Random reachable point around a center (ground snapped).
    static FVector RandomPointNear(const FVector& Center, float Radius);

    // Stuck detection + recovery. Called from the AI think step.
    static void CheckStuck(PlayerAIController& AI, float Now);

    // Occasional jumps to look natural / get over small obstacles.
    static void TryJump(PlayerAIController& AI, float Now);

    // Steering while skydiving/gliding toward the landing target.
    static void StepAirMovement(PlayerAIController& AI, float DeltaSeconds);

    // Swept-walking backend: gravity for idle pawns so nobody hovers after
    // placements, landings or teleports.
    static void SettleIdle(PlayerAIController& AI, float Now, float DeltaSeconds);
};
