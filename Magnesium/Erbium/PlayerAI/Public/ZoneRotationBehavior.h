#pragma once
// ============================================================================
// Magnesium PlayerAI - ZoneRotationBehavior
//
// Storm / safe zone awareness: knows when the AI is outside the zone, when
// the storm is closing, rotates in time (earlier with higher storm
// awareness), heals while rotating when possible, and applies fallback
// storm damage when the native storm does not damage server side AI
// entities on a version (self-calibrating to avoid double damage).
//
// TODO: connect this to the Magnesium safe zone/storm system for mobility
//       items (launch pads, rifts, vehicles) on versions that support them.
// ============================================================================
#include "PlayerAIController.h"

struct FPlayerAIWorldSnapshot;

class ZoneRotationBehavior
{
public:
    // Updates bOutsideZone and decides whether the AI must rotate now.
    // Returns true when rotation is required (overrides loot/combat wishes).
    static bool ShouldRotate(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);

    // Think step for the RotatingToZone state.
    static void Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);

    // Fallback storm damage application (every tick, cheap).
    static void TickStormDamage(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);
};
