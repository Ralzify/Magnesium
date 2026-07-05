#pragma once
// ============================================================================
// Magnesium PlayerAI - InventoryBehavior
//
// Inventory management on top of the existing Magnesium/Fortnite player
// inventory (AFortInventory). The AI picks up items, equips weapons, swaps
// weaker items for better ones, keeps ammo/heals, and drops loot on death
// through the existing death pipeline (which already handles version and
// playlist loot-drop rules). Items that do not exist on the current version
// are simply ignored.
//
// TODO: connect this to the Magnesium inventory system for finer grained
//       stack-count updates if needed on specific versions.
// ============================================================================
#include "PlayerAIController.h"

class UFortItemDefinition;
struct FFortItemEntry;

class InventoryBehavior
{
public:
    // Classifies an item definition into a generic role (close range,
    // medium range, healing, ...). Works via class + name heuristics so it
    // is not tied to one version's item pool.
    static EPlayerAIWeaponRole ClassifyItem(const UFortItemDefinition* ItemDef);

    // Gives the AI a pickup's item (respecting basic loadout logic) and
    // marks the pickup as taken. Returns true when the item was taken.
    static bool TakePickup(PlayerAIController& AI, AFortPickupAthena* Pickup);

    // Re-evaluates the AI inventory: refreshes cached role flags and equips
    // the best weapon for the current situation.
    static void RefreshAndEquipBest(PlayerAIController& AI, bool bPreferCloseRange);

    // Uses a healing / shield item (timed action handled by caller).
    // Returns the amount healed (0 when no item was available).
    static float ConsumeHealingItem(PlayerAIController& AI, bool bShield);

    // Equips a healing/shield item in hand so healing is visible to other
    // players. Returns false when no such item exists.
    static bool EquipHealingItem(PlayerAIController& AI, bool bShield);

    // True when this item would be a useful upgrade for the AI.
    static bool WantsItem(PlayerAIController& AI, const UFortItemDefinition* ItemDef);
};
