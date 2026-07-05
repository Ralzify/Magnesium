#pragma once
// ============================================================================
// Magnesium PlayerAI - MagnesiumPlayerAISpawner
//
// Spawns one PlayerAI player through the same player pipeline Magnesium
// uses for real players: player pawn + Athena player controller + player
// state, registered in the game state (GameMemberInfo), the alive players
// list and the PlayersLeft count. Because of this the existing damage,
// elimination, kill credit, kill feed, placement and win-condition systems
// treat PlayerAI players exactly like real players.
//
// This spawner is intentionally a SEPARATE implementation from the existing
// "bot" command (FortPlayerControllerAthena.cpp) - the bot command is not
// called, modified or interfered with in any way.
// ============================================================================
#include "PlayerAIEntity.h"

class PlayerAIController;

class MagnesiumPlayerAISpawner
{
public:
    // Spawns a PlayerAI on the pre-match island (random warmup player
    // start; safe fallbacks when start data is missing). Returns nullptr
    // on failure (logged, never crashes).
    static PlayerAIController* SpawnOne();

    // Spawns/replaces the pawn of an existing PlayerAI at a transform
    // (used by the transport fallback when the native aircraft path is
    // unavailable). Returns the new pawn or nullptr.
    static AFortPlayerPawnAthena* SpawnPawnAt(PlayerAIController& AI, const FVector& Location, bool bGround);

    // Post-aircraft-jump setup: wires the fresh RestartPlayer pawn like a
    // PlayerAI pawn, moves it to the aircraft and starts the native
    // skydive. Returns true when the pawn is skydiving; false when it was
    // placed at the landing target directly (no skydive support).
    static bool FinishAircraftJumpPawn(PlayerAIController& AI);

    // Fully removes a PlayerAI entity from the match (pre-match room making
    // or shutdown). Updates alive counts safely.
    static void DespawnEntity(PlayerAIController& AI, const char* Reason);
};
