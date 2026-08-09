#pragma once
// ============================================================================
// Magnesium shared support types
//
// Small, version-independent types shared by the support layer
// (VersionFeatureAdapter, AISkillProfile, AIDebugLogger) and by the bot AI
// module. Nothing here describes behavior; behavior states live with the
// system that owns them.
// ============================================================================
#include "../../../pch.h"

class AFortPlayerControllerAthena;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class AFortGameMode;
class AFortGameStateAthena;
class ABuildingContainer;
class AFortPickupAthena;

// Match phase as observed by the support layer (version independent -
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

// Skill profiles used to vary AI competence.
enum class EPlayerAISkillProfile : uint8_t
{
    Beginner,
    Average,
    Advanced,
    Aggressive,
    Passive,
    Testing, // internal / perfect-aim testing only, never picked randomly
};

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

// Small random helpers shared by the support layer and the bot AI module.
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

