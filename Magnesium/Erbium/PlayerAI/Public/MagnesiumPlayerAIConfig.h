#pragma once
// ============================================================================
// Magnesium PlayerAI - MagnesiumPlayerAIConfig
//
// Bridge between the Magnesium gameserver configuration and the PlayerAI
// system. No extra player-count settings exist: the system automatically
// uses Magnesium's existing max player count. Missing data logs the issue
// and falls back to safe defaults instead of crashing.
// ============================================================================
#include "PlayerAITypes.h"

class MagnesiumPlayerAIConfig
{
public:
    // Existing gameserver max player count (GameSession -> playlist -> 100).
    static int GetMaxPlayerCount();

    // How many PlayerAI players are wanted right now given the number of
    // real players (real players always have priority; the combined total
    // never exceeds the gameserver max player count).
    static int ComputeDesiredPlayerAICount(int RealPlayerCount);
};
