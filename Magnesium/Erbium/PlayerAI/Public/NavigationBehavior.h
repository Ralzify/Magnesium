#pragma once
// ============================================================================
// Magnesium PlayerAI - NavigationBehavior
//
// Version independent navigation. No single version specific nav system is
// relied on: movement is server side steering (velocity + ground snapping
// through the generic ground trace), with stuck detection and escalating
// recovery (repath -> nearby point -> new target -> teleport as last resort).
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
    static void TryJump(PlayerAIController& AI);

    // Steering while skydiving/gliding toward the landing target.
    static void StepAirMovement(PlayerAIController& AI, float DeltaSeconds);
};
