#pragma once
#include "SupportTypes.h"

struct FPlayerAISkillSettings
{
    EPlayerAISkillProfile Profile = EPlayerAISkillProfile::Average;

    // Combat
    float AimAccuracy = 0.45f;        // 0..1 chance for a simulated shot to hit
    float ReactionTimeSeconds = 0.9f;
    float Aggression = 0.5f;          // 0..1 overall willingness to seek fights
    float PushChance = 0.35f;         // chance to push instead of holding
    float RetreatChance = 0.35f;      // chance to retreat from a bad fight
    float ThirdPartyChance = 0.25f;   // chance to move toward nearby combat
    float EngageRange = 14000.f;
    float DetectionRange = 20000.f;

    // Movement
    float MovementQuality = 0.6f;     // 0..1, affects speed jitter / strafing
    float MoveSpeed = 520.f;          // base ground speed (uu/s)

    // Looting / inventory
    float LootGreed = 0.5f;
    float WeaponSwapSkill = 0.5f;     // chance to correctly upgrade weapons

    // Survival
    float StormAwareness = 0.6f;      // 0..1, how early the AI rotates
    float HealingDiscipline = 0.5f;   // chance to heal when safe and hurt

    // Landing
    float HotDropChance = 0.35f;      // chance to pick a busy/popular POI
};

class AISkillProfile
{
public:
    // Settings for a given named profile.
    static FPlayerAISkillSettings GetSettings(EPlayerAISkillProfile Profile);

    static EPlayerAISkillProfile PickRandomProfile();

    static const char* ToString(EPlayerAISkillProfile Profile);
};
