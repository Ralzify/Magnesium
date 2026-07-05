#pragma once
// ============================================================================
// Magnesium PlayerAI - PlayerAIEntity
//
// Thin handle around the engine objects that make up one PlayerAI player:
// player controller, player state and pawn. The entity is spawned through
// the same player pipeline real players use (see MagnesiumPlayerAISpawner)
// so damage, eliminations, kill credit, alive counts and win conditions all
// work natively.
// ============================================================================
#include "PlayerAITypes.h"
#include "../../../FortniteGame/Public/FortPlayerControllerAthena.h"

struct PlayerAIEntity
{
    // Simulated backend: a real Athena player controller.
    AFortPlayerControllerAthena* PC = nullptr;
    // Native backend: the engine's player-bot controller (Phoebe). When set,
    // PC stays null and all controller access goes through the native
    // backend's own reflection caches.
    AActor* NativeController = nullptr;
    AFortPlayerPawnAthena* NativePawn = nullptr;

    AFortPlayerStateAthena* PlayerState = nullptr;
    std::string DisplayName;
    bool bNativeBacked = false;
    bool bManualAliveCount = false; // we incremented PlayersLeft ourselves

    AFortPlayerPawnAthena* GetPawn() const;

    bool IsValid() const
    {
        return (PC != nullptr || NativeController != nullptr) && PlayerState != nullptr;
    }

    bool HasAlivePawn() const
    {
        auto Pawn = GetPawn();
        return Pawn && Pawn->GetHealth() > 0.f;
    }
};
