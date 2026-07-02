#pragma once
// ============================================================================
// Magnesium PlayerAI - AISkillProfile
//
// Internal skill profiles. Skill affects aim accuracy, reaction time,
// movement quality, loot priority, aggression, push/retreat chances,
// storm awareness, healing behavior, weapon swapping and landing choice.
// Building/editing behavior is version dependent and currently not
// simulated (safe skip).
// ============================================================================
#include "PlayerAITypes.h"

struct FPlayerAISkillSettings
{
    EPlayerAISkillProfile Profile = EPlayerAISkillProfile::Average;

    // Combat
    float AimAccuracy = 0.45f;        // 0..1 chance for a simulated shot to hit
    float ReactionTimeSeconds = 0.9f; // delay before reacting to a spotted enemy
    float Aggression = 0.5f;          // 0..1 overall willingness to seek fights
    float PushChance = 0.35f;         // chance to push instead of holding
    float RetreatChance = 0.35f;      // chance to retreat from a bad fight
    float ThirdPartyChance = 0.25f;   // chance to move toward nearby combat
    float EngageRange = 14000.f;      // max distance (uu) at which enemies are engaged
    float DetectionRange = 20000.f;   // max distance (uu) at which enemies are noticed

    // Movement
    float MovementQuality = 0.6f;     // 0..1, affects speed jitter / strafing
    float MoveSpeed = 520.f;          // base ground speed (uu/s)

    // Looting / inventory
    float LootGreed = 0.5f;           // how long the AI keeps looting vs rotating
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

    // Random profile for a newly spawned PlayerAI (never Testing).
    static EPlayerAISkillProfile PickRandomProfile();

    static const char* ToString(EPlayerAISkillProfile Profile);
};
