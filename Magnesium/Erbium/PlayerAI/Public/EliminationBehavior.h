#pragma once
// ============================================================================
// Magnesium PlayerAI - EliminationBehavior
//
// Elimination credit logging + bookkeeping. The actual elimination flow is
// the existing Magnesium pipeline (ClientOnPawnDied -> kill stats, kill
// feed, RemoveFromAlivePlayers, placement, win checks): because PlayerAI
// entities are registered as real match participants (AlivePlayers +
// PlayersLeft), a real player eliminating a PlayerAI receives normal
// elimination credit with zero extra code. This module only tracks and
// logs eliminations for debugging.
//
// TODO: connect this to the Magnesium kill credit system if additional
//       match-specific elimination tracking is added later.
// ============================================================================
#include "PlayerAIController.h"

class EliminationBehavior
{
public:
    // Called when this AI died. Logs the elimination with credit info.
    static void OnEliminated(PlayerAIController& AI, float Now);

    // Called when this AI eliminated someone (AI or real player).
    static void OnEliminatedOther(PlayerAIController& AI, AFortPlayerControllerAthena* Victim);
};
