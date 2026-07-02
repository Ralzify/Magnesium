#include "pch.h"
// ============================================================================
// Magnesium PlayerAI - PlayerAIController + PlayerAIManager
// ============================================================================
#include "../Public/PlayerAIManager.h"
#include "../Public/PlayerAIConfig.h"
#include "../Public/PlayerAIFaultGuard.h"
#include "../Public/AIDebugLogger.h"
#include "../Public/AINameGenerator.h"
#include "../Public/VersionFeatureAdapter.h"
#include "../Public/NavigationBehavior.h"
#include "../Public/PreMatchBehavior.h"
#include "../Public/TransportBehavior.h"
#include "../Public/LandingBehavior.h"
#include "../Public/LootingBehavior.h"
#include "../Public/InventoryBehavior.h"
#include "../Public/ZoneRotationBehavior.h"
#include "../Public/CombatBehavior.h"
#include "../Public/DamageBehavior.h"
#include "../Public/EliminationBehavior.h"
#include "../Public/VictoryConditionBehavior.h"
#include "../Public/MagnesiumPlayerAISpawner.h"

// ============================================================================
// PlayerAIController
// ============================================================================

bool PlayerAIController::IsAlive() const
{
    if (bDeathHandled)
        return false;

    const auto State = StateMachine.GetState();

    if (State == EPlayerAIState::Dead || State == EPlayerAIState::MatchEnded)
        return false;

    return Entity.IsValid();
}

void PlayerAIController::SetState(EPlayerAIState NewState, const char* Reason)
{
    StateMachine.Transition(NewState, VersionFeatureAdapter::GetTimeSeconds(), Entity.DisplayName.c_str(), Reason);
}

static bool PlayerAIIsGroundState(EPlayerAIState State)
{
    switch (State)
    {
    case EPlayerAIState::SearchingForLoot:
    case EPlayerAIState::MovingToLoot:
    case EPlayerAIState::OpeningContainer:
    case EPlayerAIState::PickingUpItem:
    case EPlayerAIState::ManagingInventory:
    case EPlayerAIState::RotatingToZone:
    case EPlayerAIState::SearchingForEnemies:
    case EPlayerAIState::EngagingEnemy:
    case EPlayerAIState::TakingCover:
    case EPlayerAIState::Healing:
    case EPlayerAIState::Reloading:
    case EPlayerAIState::Retreating:
    case EPlayerAIState::ThirdPartying:
    case EPlayerAIState::DownedOrDisabled:
        return true;
    default:
        return false;
    }
}

static bool PlayerAIIsMovingState(EPlayerAIState State)
{
    switch (State)
    {
    case EPlayerAIState::MovingToLoot:
    case EPlayerAIState::SearchingForLoot:
    case EPlayerAIState::RotatingToZone:
    case EPlayerAIState::SearchingForEnemies:
    case EPlayerAIState::EngagingEnemy:
    case EPlayerAIState::TakingCover:
    case EPlayerAIState::Retreating:
    case EPlayerAIState::ThirdPartying:
        return true;
    default:
        return false;
    }
}

void PlayerAIController::Update(float Now, float DeltaSeconds, FPlayerAIWorldSnapshot& World)
{
    if (!Entity.IsValid())
        return;

    const auto State = GetState();

    if (State == EPlayerAIState::Dead || State == EPlayerAIState::MatchEnded)
        return;

    // Match end freezes everyone.
    if (World.Phase == EPlayerAIMatchPhase::Ended)
    {
        NavigationBehavior::ClearMoveTarget(*this);
        SetState(EPlayerAIState::MatchEnded, "match ended");
        return;
    }

    // ---- Per tick (cheap) work ----
    if (PlayerAIIsGroundState(State))
    {
        ZoneRotationBehavior::TickStormDamage(*this, Now, World);
        DamageBehavior::DetectDeath(*this, Now);

        if (GetState() == EPlayerAIState::Dead || GetState() == EPlayerAIState::DownedOrDisabled)
        {
            if (Now >= NextThinkTime)
            {
                NextThinkTime = Now + PlayerAIRandRange(PlayerAIConfig::ThinkIntervalMin, PlayerAIConfig::ThinkIntervalMax);

                if (GetState() == EPlayerAIState::DownedOrDisabled)
                    DamageBehavior::DetectDeath(*this, Now);
            }
            return;
        }
    }

    switch (GetState())
    {
    case EPlayerAIState::PreMatchIdle:
    case EPlayerAIState::PreMatchWalking:
    case EPlayerAIState::PreMatchEmoting:
        PreMatchBehavior::Tick(*this, Now, DeltaSeconds);
        break;

    case EPlayerAIState::Gliding:
        NavigationBehavior::StepAirMovement(*this, DeltaSeconds);
        break;

    default:
        if (PlayerAIIsMovingState(GetState()) && bHasMoveTarget)
            NavigationBehavior::StepMovement(*this, Now, DeltaSeconds);
        break;
    }

    CombatBehavior::Tick(*this, Now, DeltaSeconds, World);

    // ---- Staggered decision making ----
    if (Now >= NextThinkTime)
    {
        NextThinkTime = Now + PlayerAIRandRange(PlayerAIConfig::ThinkIntervalMin, PlayerAIConfig::ThinkIntervalMax);
        Think(Now, World);
    }
}

void PlayerAIController::Think(float Now, FPlayerAIWorldSnapshot& World)
{
    // Phase transitions observed per AI (safe with late thinkers).
    if (World.Phase != ObservedPhase)
    {
        if (World.Phase == EPlayerAIMatchPhase::Transport && !bJumpedFromTransport)
        {
            TransportBehavior::OnTransportPhaseStarted(*this, Now, World);
        }
        else if (World.Phase == EPlayerAIMatchPhase::InProgress && !bJumpedFromTransport &&
                 (GetState() == EPlayerAIState::PreMatchIdle || GetState() == EPlayerAIState::PreMatchWalking ||
                  GetState() == EPlayerAIState::PreMatchEmoting || GetState() == EPlayerAIState::WaitingForTransport))
        {
            // The transport phase was skipped on this playlist/version
            // (e.g. lategame instant drop): run the transport flow now, it
            // handles missing aircraft with the landing fallback.
            TransportBehavior::OnTransportPhaseStarted(*this, Now, World);
        }

        ObservedPhase = World.Phase;
    }

    const auto State = GetState();

    switch (State)
    {
    case EPlayerAIState::PreMatchIdle:
    case EPlayerAIState::PreMatchWalking:
    case EPlayerAIState::PreMatchEmoting:
        PreMatchBehavior::Think(*this, Now, World);
        break;

    case EPlayerAIState::WaitingForTransport:
    case EPlayerAIState::InTransport:
    case EPlayerAIState::ChoosingLandingSpot:
    case EPlayerAIState::JumpingFromTransport:
        TransportBehavior::Think(*this, Now, World);
        break;

    case EPlayerAIState::Gliding:
    case EPlayerAIState::Landing:
        LandingBehavior::Think(*this, Now, World);
        break;

    case EPlayerAIState::SearchingForLoot:
    case EPlayerAIState::MovingToLoot:
    case EPlayerAIState::OpeningContainer:
    case EPlayerAIState::PickingUpItem:
    case EPlayerAIState::ManagingInventory:
    {
        // Being shot at? Fight back before looting on.
        if (LastDamagerPC && Now - LastDamageTime < 4.f && CombatBehavior::IsValidEnemy(*this, LastDamagerPC))
        {
            CombatTarget = LastDamagerPC;
            TargetLastSeenTime = Now;
            ReactionReadyTime = Now + Skill.ReactionTimeSeconds;
            SetState(EPlayerAIState::EngagingEnemy, "under fire while looting");
            break;
        }

        // Storm has priority over loot.
        if (ZoneRotationBehavior::ShouldRotate(*this, Now, World))
        {
            NavigationBehavior::ClearMoveTarget(*this);
            LootContainerTarget = nullptr;
            LootPickupTarget = nullptr;
            SetState(EPlayerAIState::RotatingToZone, "storm pressure");
            break;
        }

        // Opportunistic combat while looting.
        if (State == EPlayerAIState::SearchingForLoot && PlayerAIRandChance(Skill.Aggression * 0.5f))
        {
            auto Enemy = CombatBehavior::DetectEnemy(*this, World);

            if (Enemy)
            {
                CombatTarget = Enemy;
                TargetLastSeenTime = Now;
                ReactionReadyTime = Now + Skill.ReactionTimeSeconds;
                InventoryBehavior::RefreshAndEquipBest(*this, false);
                SetState(EPlayerAIState::EngagingEnemy, "enemy while looting");
                break;
            }
        }

        LootingBehavior::Think(*this, Now, World);
        break;
    }

    case EPlayerAIState::RotatingToZone:
        ZoneRotationBehavior::Think(*this, Now, World);
        break;

    case EPlayerAIState::SearchingForEnemies:
    case EPlayerAIState::EngagingEnemy:
    case EPlayerAIState::TakingCover:
    case EPlayerAIState::Healing:
    case EPlayerAIState::Reloading:
    case EPlayerAIState::Retreating:
    case EPlayerAIState::ThirdPartying:
    {
        // Survival beats fighting: leave long fights when the storm closes.
        if (State != EPlayerAIState::Healing && ZoneRotationBehavior::ShouldRotate(*this, Now, World))
        {
            const bool bCommitted = State == EPlayerAIState::EngagingEnemy && PlayerAIRandChance(Skill.PushChance * 0.5f);

            if (!bCommitted)
            {
                CombatState = EPlayerAICombatState::RotatingAway;
                CombatTarget = nullptr;
                NavigationBehavior::ClearMoveTarget(*this);
                SetState(EPlayerAIState::RotatingToZone, "storm beats fighting");
                break;
            }
        }

        CombatBehavior::Think(*this, Now, World);
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// PlayerAIManager
// ============================================================================

void PlayerAIManager::Initialize()
{
    if (bInitialized)
        return;

    bInitialized = true;
    EliminatedCount = 0;
    NextAIIndex = 1;
    AINameGenerator::Reset();
    VersionFeatureAdapter::ResetCaches();
    LandingBehavior::Reset();

    AIDebugLogger::Log("Manager", "PlayerAI system initialized (max player count: %d)", VersionFeatureAdapter::GetMaxPlayerCount());
}

void PlayerAIManager::Shutdown(const char* Reason)
{
    if (!bInitialized && Controllers.empty())
        return;

    RemoveAll(Reason);
    bInitialized = false;

    AIDebugLogger::Log("Manager", "PlayerAI system shut down (%s)", Reason ? Reason : "");
}

void PlayerAIManager::OnMatchEnded()
{
    AIDebugLogger::Log("Manager", "match ended - cleaning up %d PlayerAI players (%d were eliminated)",
        (int)Controllers.size(), EliminatedCount);

    RemoveAll("match ended");
}

void PlayerAIManager::RefreshWorldSnapshot(float Now)
{
    World.Now = Now;
    World.GameMode = VersionFeatureAdapter::GetGameMode();
    World.GameState = VersionFeatureAdapter::GetGameState();
    World.Phase = VersionFeatureAdapter::GetMatchPhase();
    World.bHasSafeZone = VersionFeatureAdapter::TryGetSafeZone(World.SafeZoneCenter, World.SafeZoneRadius);

    if (Now - World.LastLootScanTime >= PlayerAIConfig::WorldSnapshotInterval)
    {
        World.LastLootScanTime = Now;

        World.Containers.clear();
        World.Pickups.clear();

        // Generic loot sources: anything derived from BuildingContainer
        // (chests, ammo boxes, supply containers, version equivalents) and
        // world pickups (floor loot, dropped items).
        TArray<ABuildingContainer*> Containers;
        Utils::GetAll<ABuildingContainer>(Containers);

        for (auto& Container : Containers)
        {
            if (!Container)
                continue;

            if (Container->HasbAlreadySearched() && Container->bAlreadySearched)
                continue;

            World.Containers.push_back(Container);
        }

        Containers.Free();

        TArray<AFortPickupAthena*> Pickups;
        Utils::GetAll<AFortPickupAthena>(Pickups);

        for (auto& Pickup : Pickups)
        {
            if (!Pickup)
                continue;

            if (Pickup->HasbPickedUp() && Pickup->bPickedUp)
                continue;

            World.Pickups.push_back(Pickup);
        }

        Pickups.Free();
    }
}

// Last-resort fault containment: no PlayerAI update is ever allowed to
// crash the gameserver. A faulting AI is retried, then quarantined (frozen)
// after repeated faults - the match keeps running either way.
// (Kept free of unwindable C++ objects so SEH is allowed here.)
static bool PlayerAITryUpdate(PlayerAIController* Controller, float Now, float DeltaSeconds, FPlayerAIWorldSnapshot& World)
{
    GPlayerAIGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        Controller->Update(Now, DeltaSeconds, World);
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GPlayerAIGuardedNativeCallDepth--;
    return bOk;
}

void PlayerAIManager::UpdateAll(float Now, float DeltaSeconds)
{
    if (!bInitialized)
        return;

    RefreshWorldSnapshot(Now);

    for (auto& Controller : Controllers)
    {
        if (!Controller)
            continue;

        if (PlayerAITryUpdate(Controller.get(), Now, DeltaSeconds, World))
            continue;

        Controller->FaultCount++;
        AIDebugLogger::Error("Manager", "AIPlayer '%s' update faulted (%d/3) - contained",
            Controller->Entity.DisplayName.c_str(), Controller->FaultCount);

        if (Controller->FaultCount >= 3)
        {
            // Quarantine: stop driving this AI. Its pawn stays a normal,
            // damageable player pawn, so the match still resolves natively.
            Controller->bDeathHandled = true;
            Controller->SetState(EPlayerAIState::MatchEnded, "quarantined after repeated faults");
            AIDebugLogger::Error("Manager", "AIPlayer '%s' quarantined to protect the gameserver",
                Controller->Entity.DisplayName.c_str());
        }
    }

    // Periodic alive count debug line.
    if (Now - LastAliveLogTime > 30.f && !Controllers.empty() && World.Phase >= EPlayerAIMatchPhase::Transport)
    {
        LastAliveLogTime = Now;
        auto Counts = VictoryConditionBehavior::UpdateAliveCounts(World);
        AIDebugLogger::Log("AliveCount", "Total=%d Real=%d PlayerAI=%d",
            Counts.TotalAlivePlayers, Counts.AliveRealPlayers, Counts.AlivePlayerAIs);
    }
}

PlayerAIController* PlayerAIManager::RegisterEntity(const PlayerAIEntity& Entity)
{
    auto Controller = std::make_unique<PlayerAIController>();

    Controller->Entity = Entity;
    Controller->AIIndex = NextAIIndex++;
    Controller->Skill = AISkillProfile::GetSettings(AISkillProfile::PickRandomProfile());
    Controller->SpawnedAtTime = VersionFeatureAdapter::GetTimeSeconds();
    Controller->NextThinkTime = Controller->SpawnedAtTime + PlayerAIRandRange(0.2f, 1.2f);
    Controller->ObservedPhase = World.Phase;

    if (auto Pawn = Controller->GetPawn())
    {
        Controller->HomeLocation = Pawn->K2_GetActorLocation();
        Controller->CachedGroundZ = (float)Controller->HomeLocation.Z - 90.f;
    }

    Controller->SetState(EPlayerAIState::PreMatchIdle, "spawned");
    Controller->PreMatchActionEndTime = Controller->SpawnedAtTime + PlayerAIRandRange(0.5f, 3.f);

    auto Raw = Controller.get();
    Controllers.push_back(std::move(Controller));

    AIDebugLogger::Log("Manager", "spawned AIPlayer '%s' (#%d, skill %s) - %d total",
        Raw->Entity.DisplayName.c_str(), Raw->AIIndex, AISkillProfile::ToString(Raw->Skill.Profile), (int)Controllers.size());

    return Raw;
}

bool PlayerAIManager::RemoveOnePreMatch()
{
    for (int i = (int)Controllers.size() - 1; i >= 0; i--)
    {
        auto& Controller = Controllers[i];

        if (!Controller)
            continue;

        const auto State = Controller->GetState();

        if (State != EPlayerAIState::PreMatchIdle && State != EPlayerAIState::PreMatchWalking &&
            State != EPlayerAIState::PreMatchEmoting)
            continue;

        AIDebugLogger::Log("Manager", "removing AIPlayer '%s' to make room for a real player", Controller->Entity.DisplayName.c_str());

        MagnesiumPlayerAISpawner::DespawnEntity(*Controller, "real player priority");
        Controllers.erase(Controllers.begin() + i);
        return true;
    }

    return false;
}

void PlayerAIManager::RemoveAll(const char* Reason)
{
    for (auto& Controller : Controllers)
    {
        if (Controller)
            MagnesiumPlayerAISpawner::DespawnEntity(*Controller, Reason);
    }

    Controllers.clear();
    AINameGenerator::Reset();
    LandingBehavior::Reset();
    EliminatedCount = 0;
}

int PlayerAIManager::GetTotalCount()
{
    return (int)Controllers.size();
}

int PlayerAIManager::GetAliveCount()
{
    int Alive = 0;

    for (auto& Controller : Controllers)
        if (Controller && Controller->IsAlive())
            Alive++;

    return Alive;
}

int PlayerAIManager::GetEliminatedCount()
{
    return EliminatedCount;
}

bool PlayerAIManager::IsPlayerAI(AFortPlayerControllerAthena* PC)
{
    return FindByController(PC) != nullptr;
}

PlayerAIController* PlayerAIManager::FindByController(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return nullptr;

    for (auto& Controller : Controllers)
        if (Controller && Controller->Entity.PC == PC)
            return Controller.get();

    return nullptr;
}
