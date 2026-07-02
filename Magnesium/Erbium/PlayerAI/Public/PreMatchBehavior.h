#pragma once
// ============================================================================
// Magnesium PlayerAI - PreMatchBehavior
//
// Pre-match island behavior: natural wandering, idling, turning, jumping
// and emoting (emotes only when the version supports them - when they do
// not exist the AI simply keeps walking/idling/jumping, no fallback
// animations, no crashes).
// ============================================================================
#include "PlayerAIController.h"

struct FPlayerAIWorldSnapshot;

class PreMatchBehavior
{
public:
    // Think step while in a PreMatch* state.
    static void Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);

    // Per tick movement while pre-match.
    static void Tick(PlayerAIController& AI, float Now, float DeltaSeconds);
};
