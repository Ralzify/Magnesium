#pragma once
// ============================================================================
// Magnesium PlayerAI - LootingBehavior
//
// Floor loot + container looting. Containers are handled generically
// (anything derived from BuildingContainer: chests, ammo boxes, supply
// containers, version specific equivalents). Unknown container types and
// unsupported items are skipped safely, never crashing the AI.
// ============================================================================
#include "PlayerAIController.h"

struct FPlayerAIWorldSnapshot;

class LootingBehavior
{
public:
    // Think step for SearchingForLoot / MovingToLoot / OpeningContainer /
    // PickingUpItem states.
    static void Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);

    // Finds the closest interesting loot (container or pickup) near the AI.
    // Returns false when nothing is in range.
    static bool AcquireLootTarget(PlayerAIController& AI, FPlayerAIWorldSnapshot& World);
};
