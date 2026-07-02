#pragma once
// ============================================================================
// Magnesium PlayerAI - CombatBehavior
//
// Combat against both real human players and other PlayerAI players using
// ONE shared enemy detection path (GameMode->AlivePlayers). Aim is skill
// based (accuracy, reaction time, randomness) - never unfairly perfect
// unless the internal Testing profile is used. Combat never touches the
// combat logic of native henchmen/guards/bosses/wildlife.
//
// TODO: connect simulated weapon fire to the Magnesium damage system /
//       real weapon abilities per version for full fire cosmetics.
// ============================================================================
#include "PlayerAIController.h"

struct FPlayerAIWorldSnapshot;

class CombatBehavior
{
public:
    // Shared enemy detection for humans and AI. Returns nullptr when no
    // enemy is in detection range.
    static AFortPlayerControllerAthena* DetectEnemy(PlayerAIController& AI, FPlayerAIWorldSnapshot& World);

    // Think step for combat states (SearchingForEnemies, EngagingEnemy,
    // TakingCover, Reloading, Retreating, ThirdPartying).
    static void Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);

    // Per tick combat update (aiming + simulated shots).
    static void Tick(PlayerAIController& AI, float Now, float DeltaSeconds, FPlayerAIWorldSnapshot& World);

    // True when the target is still a valid, alive, non-teammate enemy.
    static bool IsValidEnemy(PlayerAIController& AI, AFortPlayerControllerAthena* Enemy);
};
