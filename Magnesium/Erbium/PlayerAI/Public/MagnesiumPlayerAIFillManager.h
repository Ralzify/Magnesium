#pragma once
// ============================================================================
// Magnesium PlayerAI - MagnesiumPlayerAIFillManager
//
// Automatic lobby fill: once at least one real human player has joined the
// pre-match lobby, PlayerAI players are spawned until the lobby reaches
// Magnesium's existing max player count. Real players always have priority:
// the fill never exceeds the max player count, never blocks real players
// from joining, and frees slots (prevent further spawns / safely remove
// PlayerAI players) when more real players join before the match starts.
// ============================================================================
#include "PlayerAITypes.h"

class MagnesiumPlayerAIFillManager
{
public:
    // Called every server tick while the system is initialized. Filling is
    // armed only once the first real player has actually spawned onto the
    // pre-match island (bAnyRealPlayerSpawned), never during a login.
    static void Tick(float Now, int RealPlayerCount, bool bAnyRealPlayerSpawned, EPlayerAIMatchPhase Phase);

    // Called when a real player joins/leaves (adjusts fill immediately).
    static void OnRealPlayerJoined(float Now, int RealPlayerCount);
    static void OnRealPlayerLeft(float Now, int RealPlayerCount);

    static void Reset();

private:
    static inline bool bFillStarted = false;
    static inline float FillAllowedAtTime = 0.f;
};
