#pragma once
// ============================================================================
// Magnesium PlayerAI - VictoryConditionBehavior
//
// Alive count / victory condition bookkeeping. PlayerAI players count as
// real match participants (they are inside GameMode->AlivePlayers and
// GameState->PlayersLeft), so the existing Magnesium win-condition logic
// already treats them correctly: killing the final PlayerAI triggers the
// same victory flow as killing the final real player. This module tracks
// TotalAlivePlayers / AliveRealPlayers / AlivePlayerAIs and logs win checks.
//
// TODO: connect this to the Magnesium win-condition system if custom match
//       rules are added later.
// ============================================================================
#include "PlayerAITypes.h"

struct FPlayerAIWorldSnapshot;

class VictoryConditionBehavior
{
public:
    struct FAliveCounts
    {
        int TotalAlivePlayers = 0;
        int AliveRealPlayers = 0;
        int AlivePlayerAIs = 0;
    };

    // Recomputes and logs alive counts (called when a PlayerAI dies and
    // periodically for debugging).
    static FAliveCounts UpdateAliveCounts(FPlayerAIWorldSnapshot& World);

    static FAliveCounts GetLastCounts() { return LastCounts; }

private:
    static inline FAliveCounts LastCounts{};
};
