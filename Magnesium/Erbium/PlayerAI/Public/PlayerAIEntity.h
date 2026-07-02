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
    AFortPlayerControllerAthena* PC = nullptr;
    AFortPlayerStateAthena* PlayerState = nullptr;
    std::string DisplayName;

    AFortPlayerPawnAthena* GetPawn() const
    {
        if (!PC)
            return nullptr;
        auto Pawn = PC->MyFortPawn;
        if (!Pawn)
            Pawn = PC->Pawn;
        return Pawn;
    }

    bool IsValid() const
    {
        return PC != nullptr && PlayerState != nullptr;
    }

    bool HasAlivePawn() const
    {
        auto Pawn = GetPawn();
        return Pawn && Pawn->GetHealth() > 0.f;
    }
};
