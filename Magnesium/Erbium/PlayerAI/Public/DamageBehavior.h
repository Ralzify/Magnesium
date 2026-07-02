#pragma once
// ============================================================================
// Magnesium PlayerAI - DamageBehavior
//
// Damage application for simulated PlayerAI attacks. No fake separate death
// system exists: PlayerAI players have normal health/shield, take damage
// from every native source like real players (their pawns are real player
// pawns), and fatal damage always goes through the native ForceKill death
// pipeline so kill credit / kill feed / stats / placement work natively.
//
// TODO: connect this to the Magnesium damage system (gameplay effects) for
//       per-version damage cosmetics (hit markers, damage numbers).
// ============================================================================
#include "PlayerAIController.h"

class DamageBehavior
{
public:
    // Applies simulated weapon damage from Attacker to VictimPC's pawn:
    // shield first, then health; fatal damage is routed through the native
    // death pipeline with proper killer attribution.
    // Returns true when the victim died.
    static bool ApplyWeaponDamage(PlayerAIController& Attacker, AFortPlayerControllerAthena* VictimPC, float Damage);

    // Applies environment (storm) damage to an AI pawn - no kill credit.
    // Returns true when the AI died.
    static bool ApplyEnvironmentalDamage(PlayerAIController& AI, float Damage);

    // Detects that this AI's pawn died from ANY source (native or simulated)
    // and moves the controller into the Dead state exactly once.
    static void DetectDeath(PlayerAIController& AI, float Now);
};
