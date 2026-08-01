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

    AFortPlayerPawnAthena* GetPawn() const;
    // Returns only the pawn currently owned by the native bot controller.
    // Unlike GetPawn(), this never falls back to the remembered spawn pawn;
    // lifecycle validation uses it to detect an unauthorized replacement.
    AFortPlayerPawnAthena* GetNativeControllerPawn() const;
    AFortInventory* GetInventory() const;

    bool HasLiveController() const;
    bool HasLivePlayerState() const;
    bool IsValid() const;
    static bool IsLivePawn(const AFortPlayerPawnAthena* Pawn);

    bool HasAlivePawn() const
    {
        auto Pawn = GetPawn();
        return Pawn && Pawn->GetHealth() > 0.f;
    }
};
