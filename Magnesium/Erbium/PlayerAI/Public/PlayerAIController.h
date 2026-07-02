#pragma once
// ============================================================================
// Magnesium PlayerAI - PlayerAIController
//
// One controller ("brain") per PlayerAI player. Tracks the current match
// phase, behavior state, target, path, landing target, combat decision,
// looting target, last damage source, elimination credit data, stuck
// detection and debug information. Behavior modules (PreMatchBehavior,
// CombatBehavior, ...) operate on this controller.
// ============================================================================
#include "PlayerAITypes.h"
#include "PlayerAIEntity.h"
#include "AISkillProfile.h"
#include "PlayerAIStateMachine.h"

struct FPlayerAIWorldSnapshot;

class PlayerAIController
{
public:
    // ---- Identity / engine handles ----
    PlayerAIEntity Entity;
    int AIIndex = 0;
    FPlayerAISkillSettings Skill;

    // ---- State machine ----
    PlayerAIStateMachine StateMachine;
    EPlayerAICombatState CombatState = EPlayerAICombatState::Searching;
    EPlayerAIMatchPhase ObservedPhase = EPlayerAIMatchPhase::None;

    // ---- Movement / navigation ----
    FVector MoveTarget{};            // current waypoint the AI walks toward
    bool bHasMoveTarget = false;
    FVector HomeLocation{};          // pre-match anchor to wander around
    float CachedGroundZ = 0.f;
    float NextGroundSnapTime = 0.f;
    float NextThinkTime = 0.f;

    // ---- Stuck detection ----
    FVector LastStuckCheckLocation{};
    float NextStuckCheckTime = 0.f;
    int StuckCounter = 0;

    // ---- Pre match ----
    float PreMatchActionEndTime = 0.f;
    float EmoteEndTime = 0.f;

    // ---- Transport / landing ----
    FVector LandingTarget{};
    bool bHasLandingTarget = false;
    bool bEnteredTransport = false;
    bool bVirtualTransport = false;  // native aircraft unavailable -> simulated seat
    bool bThankedDriver = false;
    bool bJumpedFromTransport = false;
    float TransportPhaseStartTime = 0.f;
    float ForcedJumpTime = 0.f;
    float ThankDriverTime = 0.f;
    float JumpedAtTime = 0.f;

    // ---- Looting ----
    ABuildingContainer* LootContainerTarget = nullptr;
    AFortPickupAthena* LootPickupTarget = nullptr;
    FVector LootTargetLocation{};
    float ActionEndTime = 0.f;       // generic timed action (open container, heal, reload)
    float LootingSinceTime = 0.f;

    // ---- Inventory (loose simulated tracking) ----
    FGuid EquippedItemGuid{};
    EPlayerAIWeaponRole EquippedRole = EPlayerAIWeaponRole::None;
    int MagazineRemaining = 0;
    bool bHasCloseRange = false;
    bool bHasMediumRange = false;
    int HealingItemCount = 0;
    int ShieldItemCount = 0;

    // ---- Combat ----
    AFortPlayerControllerAthena* CombatTarget = nullptr;
    FVector LastKnownTargetLocation{};
    float TargetLastSeenTime = 0.f;
    float ReactionReadyTime = 0.f;   // aim delay after spotting
    float NextShotTime = 0.f;

    // ---- Storm ----
    bool bOutsideZone = false;
    float OutsideZoneSince = 0.f;
    float LastObservedHealth = 0.f;
    float LastStormDamageTime = 0.f;
    bool bStormFallbackActive = false;

    // ---- Death / elimination credit data ----
    AFortPlayerControllerAthena* LastDamagerPC = nullptr; // last player/AI that damaged us (via simulated combat)
    float LastDamageTime = 0.f;
    float DBNOSince = 0.f;
    bool bDeathHandled = false;

    // ---- Debug / stability ----
    float SpawnedAtTime = 0.f;
    int FaultCount = 0; // contained native faults; quarantined after 3

    // Per tick update, driven by PlayerAIManager from the server tick.
    void Update(float Now, float DeltaSeconds, FPlayerAIWorldSnapshot& World);

    // Helpers
    AFortPlayerPawnAthena* GetPawn() const { return Entity.GetPawn(); }
    bool IsAlive() const;
    EPlayerAIState GetState() const { return StateMachine.GetState(); }
    void SetState(EPlayerAIState NewState, const char* Reason);

private:
    void Think(float Now, FPlayerAIWorldSnapshot& World);
};
