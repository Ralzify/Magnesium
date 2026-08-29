#pragma once
#include "../../../pch.h"

class AFortPlayerControllerAthena;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class AFortGameMode;
class AFortGameStateAthena;
class ABuildingContainer;
class AFortPickupAthena;

enum class EPlayerAIMatchPhase : uint8_t
{
    None, WaitingForServer, PreMatch, Transport, InProgress, Ended,
};

enum class EPlayerAISkillProfile : uint8_t
{
    Beginner, Average, Advanced, Aggressive, Passive, Testing,
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
