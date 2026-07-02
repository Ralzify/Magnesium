#pragma once
// ============================================================================
// Magnesium PlayerAI - Shared types
//
// Universal, version-independent PlayerAI system. These AI players are
// simulated real players ("PlayerAI" / "AIPlayer"), NOT the existing
// "bot" command (which only spawns a static player pawn) and NOT native
// henchmen / guards / bosses / wildlife / NPC AI.
//
// This module never touches the existing bot command, player pawn spawn
// commands, or native AI managers.
// ============================================================================
#include "../../../pch.h"

class AFortPlayerControllerAthena;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class AFortGameMode;
class AFortGameStateAthena;
class ABuildingContainer;
class AFortPickupAthena;

// Full-match behavior states for a PlayerAI.
enum class EPlayerAIState : uint8_t
{
    PreMatchIdle,
    PreMatchWalking,
    PreMatchEmoting,
    WaitingForTransport,
    InTransport,
    ChoosingLandingSpot,
    JumpingFromTransport,
    Gliding,
    Landing,
    SearchingForLoot,
    MovingToLoot,
    OpeningContainer,
    PickingUpItem,
    ManagingInventory,
    RotatingToZone,
    SearchingForEnemies,
    EngagingEnemy,
    TakingCover,
    Healing,
    Reloading,
    Retreating,
    ThirdPartying,
    DownedOrDisabled,
    Dead,
    MatchEnded,
};

// Combat sub-states used while a PlayerAI is fighting.
enum class EPlayerAICombatState : uint8_t
{
    Searching,
    SpottingEnemy,
    Engaging,
    Pushing,
    TakingCover,
    Reloading,
    Healing,
    Retreating,
    ThirdPartying,
    Disengaging,
    RotatingAway,
};

// Match phase as observed by the PlayerAI system (version independent -
// derived from the gameserver state, never from one hardcoded enum).
enum class EPlayerAIMatchPhase : uint8_t
{
    None,
    WaitingForServer,
    PreMatch,
    Transport,
    InProgress,
    Ended,
};

// Internal skill profiles.
enum class EPlayerAISkillProfile : uint8_t
{
    Beginner,
    Average,
    Advanced,
    Aggressive,
    Passive,
    Testing, // internal / perfect-aim testing only, never picked randomly
};

// Broad weapon roles the inventory/combat logic reasons about. Mapped from
// item definitions generically so it works across item pools and versions.
enum class EPlayerAIWeaponRole : uint8_t
{
    None,
    CloseRange,   // shotguns, SMGs
    MediumRange,  // ARs, pistols
    LongRange,    // snipers, DMRs
    Explosive,
    Healing,
    Shield,
    Ammo,
    Utility,
    Resource,
    Unknown,
};

inline const char* PlayerAIStateToString(EPlayerAIState State)
{
    switch (State)
    {
    case EPlayerAIState::PreMatchIdle: return "PreMatchIdle";
    case EPlayerAIState::PreMatchWalking: return "PreMatchWalking";
    case EPlayerAIState::PreMatchEmoting: return "PreMatchEmoting";
    case EPlayerAIState::WaitingForTransport: return "WaitingForTransport";
    case EPlayerAIState::InTransport: return "InTransport";
    case EPlayerAIState::ChoosingLandingSpot: return "ChoosingLandingSpot";
    case EPlayerAIState::JumpingFromTransport: return "JumpingFromTransport";
    case EPlayerAIState::Gliding: return "Gliding";
    case EPlayerAIState::Landing: return "Landing";
    case EPlayerAIState::SearchingForLoot: return "SearchingForLoot";
    case EPlayerAIState::MovingToLoot: return "MovingToLoot";
    case EPlayerAIState::OpeningContainer: return "OpeningContainer";
    case EPlayerAIState::PickingUpItem: return "PickingUpItem";
    case EPlayerAIState::ManagingInventory: return "ManagingInventory";
    case EPlayerAIState::RotatingToZone: return "RotatingToZone";
    case EPlayerAIState::SearchingForEnemies: return "SearchingForEnemies";
    case EPlayerAIState::EngagingEnemy: return "EngagingEnemy";
    case EPlayerAIState::TakingCover: return "TakingCover";
    case EPlayerAIState::Healing: return "Healing";
    case EPlayerAIState::Reloading: return "Reloading";
    case EPlayerAIState::Retreating: return "Retreating";
    case EPlayerAIState::ThirdPartying: return "ThirdPartying";
    case EPlayerAIState::DownedOrDisabled: return "DownedOrDisabled";
    case EPlayerAIState::Dead: return "Dead";
    case EPlayerAIState::MatchEnded: return "MatchEnded";
    }
    return "Unknown";
}

inline const char* PlayerAICombatStateToString(EPlayerAICombatState State)
{
    switch (State)
    {
    case EPlayerAICombatState::Searching: return "Searching";
    case EPlayerAICombatState::SpottingEnemy: return "SpottingEnemy";
    case EPlayerAICombatState::Engaging: return "Engaging";
    case EPlayerAICombatState::Pushing: return "Pushing";
    case EPlayerAICombatState::TakingCover: return "TakingCover";
    case EPlayerAICombatState::Reloading: return "Reloading";
    case EPlayerAICombatState::Healing: return "Healing";
    case EPlayerAICombatState::Retreating: return "Retreating";
    case EPlayerAICombatState::ThirdPartying: return "ThirdPartying";
    case EPlayerAICombatState::Disengaging: return "Disengaging";
    case EPlayerAICombatState::RotatingAway: return "RotatingAway";
    }
    return "Unknown";
}

inline const char* PlayerAIMatchPhaseToString(EPlayerAIMatchPhase Phase)
{
    switch (Phase)
    {
    case EPlayerAIMatchPhase::None: return "None";
    case EPlayerAIMatchPhase::WaitingForServer: return "WaitingForServer";
    case EPlayerAIMatchPhase::PreMatch: return "PreMatch";
    case EPlayerAIMatchPhase::Transport: return "Transport";
    case EPlayerAIMatchPhase::InProgress: return "InProgress";
    case EPlayerAIMatchPhase::Ended: return "Ended";
    }
    return "Unknown";
}

// Small random helpers shared by the PlayerAI modules.
inline float PlayerAIRandRange(float Min, float Max)
{
    if (Max <= Min)
        return Min;
    return Min + (Max - Min) * (float(rand()) / float(RAND_MAX));
}

inline int PlayerAIRandInt(int Min, int Max)
{
    if (Max <= Min)
        return Min;
    return Min + rand() % (Max - Min + 1);
}

inline bool PlayerAIRandChance(float Chance01)
{
    return PlayerAIRandRange(0.f, 1.f) < Chance01;
}
