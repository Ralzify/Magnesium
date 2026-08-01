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

namespace
{
    // Shared spawn recovery is intentionally tiny. A broken pawn path on an
    // unsupported build must not let a filled lobby call RestartPlayer in one
    // TickFlush.
    constexpr int PlayerAIPawnRecoveryBudgetPerTick = 2;
    constexpr int PlayerAIMaxPawnRecoveryAttempts = 6;
    int PlayerAIPawnRecoveryBudgetRemaining = 0;
    constexpr int PlayerAITransportJumpBudgetPerTick = 2;
    int PlayerAITransportJumpBudgetRemaining = 0;

    bool PlayerAITryConsumePawnRecoveryBudget()
    {
        if (PlayerAIPawnRecoveryBudgetRemaining <= 0)
            return false;

        PlayerAIPawnRecoveryBudgetRemaining--;
        return true;
    }

    // Loot discovery walks only this many global object slots per server
    // tick. Results remain staged until a complete pass, so behavior code
    // always observes one coherent cache.
    constexpr int PlayerAILootDiscoveryBudgetPerTick = 512;
    UWorld* PlayerAILootScanWorld = nullptr;
    int PlayerAILootScanCursor = 0;
    int PlayerAILootScanLimit = 0;
    bool bPlayerAILootScanActive = false;
    std::vector<ABuildingContainer*> PlayerAIPendingContainers;
    std::vector<AFortPickupAthena*> PlayerAIPendingPickups;

    bool PlayerAIIsLiveObject(const UObject* Object)
    {
        if (!Object ||
            !SDK::MemReadable(Object, sizeof(UObject)))
        {
            return false;
        }

        const int32 ObjectIndex = Object->Index;
        const int32 ObjectCount = TUObjectArray::Num();

        if (ObjectIndex < 0 || ObjectIndex >= ObjectCount)
            return false;

        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        const int32 InvalidObjectFlags =
            Offsets::bEncryptedObjects ? 0x10200000 : 0x20;

        return Item &&
            Item->GetObject() == Object &&
            !(Item->GetFlags() & InvalidObjectFlags) &&
            Object->Class &&
            SDK::MemReadable(Object->Class, sizeof(UClass));
    }

    bool PlayerAIObjectBelongsToWorld(
        const UObject* Object,
        const UWorld* ExpectedWorld)
    {
        if (!ExpectedWorld)
            return false;

        // Actors are outered to their ULevel, whose outer chain reaches the
        // owning UWorld on every supported UE4 layout. Bound the walk and
        // validate every hop so stale cache entries are never dereferenced.
        const UObject* Current = Object;

        for (int Depth = 0; Depth < 8 && Current; Depth++)
        {
            if (Current == ExpectedWorld)
                return true;

            if (!PlayerAIIsLiveObject(Current))
                return false;

            Current = Current->Outer;
        }

        return false;
    }

    void PlayerAIResetLootDiscovery(bool bClearPublished)
    {
        PlayerAILootScanWorld = nullptr;
        PlayerAILootScanCursor = 0;
        PlayerAILootScanLimit = 0;
        bPlayerAILootScanActive = false;
        PlayerAIPendingContainers.clear();
        PlayerAIPendingPickups.clear();

        if (bClearPublished)
        {
            PlayerAIManager::GetWorld().Containers.clear();
            PlayerAIManager::GetWorld().Pickups.clear();
            PlayerAIManager::GetWorld().LastLootScanTime = -1000.f;
        }
    }
}

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

void PlayerAIController::SetTransitionDamageProtection(bool bProtect)
{
    auto Pawn = GetPawn();

    // Never restore a flag on an old/destroyed pawn, and never claim
    // ownership of invulnerability that another gameplay system applied.
    if (DamageProtectedPawn != Pawn)
    {
        DamageProtectedPawn = nullptr;
        bTransitionDamageProtectionApplied = false;
    }

    if (!Pawn || !Pawn->HasbCanBeDamaged())
        return;

    if (bProtect)
    {
        if (bTransitionDamageProtectionApplied &&
            DamageProtectedPawn == Pawn)
        {
            // Possession/skydive setup can restore this flag after we first
            // acquired it. Reassert protection every transition tick while
            // retaining the same ownership rules for the eventual restore.
            if (Pawn->bCanBeDamaged)
                Pawn->bCanBeDamaged = false;

            return;
        }

        if (Pawn->bCanBeDamaged)
        {
            Pawn->bCanBeDamaged = false;
            DamageProtectedPawn = Pawn;
            bTransitionDamageProtectionApplied = true;
        }

        return;
    }

    if (bTransitionDamageProtectionApplied &&
        DamageProtectedPawn == Pawn &&
        !Pawn->bCanBeDamaged)
    {
        Pawn->bCanBeDamaged = true;
    }

    DamageProtectedPawn = nullptr;
    bTransitionDamageProtectionApplied = false;
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

static bool PlayerAIIsNativeAircraftHandoffWindow(
    const PlayerAIController& AI,
    const FPlayerAIWorldSnapshot& World)
{
    if (AI.bNativePawnIdentityLocked ||
        AI.bNativeAircraftHandoffConsumed ||
        AI.bJumpedFromTransport ||
        AI.NativePawnGeneration != 1)
    {
        return false;
    }

    if (World.Phase != EPlayerAIMatchPhase::Transport &&
        World.Phase != EPlayerAIMatchPhase::InProgress)
    {
        return false;
    }

    switch (AI.GetState())
    {
    case EPlayerAIState::PreMatchIdle:
    case EPlayerAIState::PreMatchWalking:
    case EPlayerAIState::PreMatchEmoting:
        // Validation runs before phase-transition setup in Update. Permit the
        // same single handoff while that setup is newly pending, including
        // playlists which skip the explicit Transport phase.
        return AI.bTransportSetupPending ||
            AI.ObservedPhase != World.Phase;
    case EPlayerAIState::WaitingForTransport:
    case EPlayerAIState::InTransport:
    case EPlayerAIState::ChoosingLandingSpot:
        return true;
    default:
        return false;
    }
}

static constexpr float PlayerAINativePawnHandoffGraceSeconds = 2.f;

static void PlayerAINormalizeTerminalPawnDamage(
    AFortPlayerPawnAthena* Pawn)
{
    if (!PlayerAIEntity::IsLivePawn(Pawn) ||
        !Pawn->HasbCanBeDamaged() ||
        Pawn->bCanBeDamaged)
    {
        return;
    }

    // Teardown should follow immediately. Restoring damage first ensures a
    // replacement cannot remain as an invulnerable world actor if an engine
    // destroy request is deferred by one frame.
    Pawn->bCanBeDamaged = true;
    Pawn->ForceNetUpdate();
}

bool PlayerAIController::ValidateNativePawnIdentity(
    float Now, FPlayerAIWorldSnapshot& World)
{
    if (!Entity.bNativeBacked)
        return true;

    auto CurrentPawn = Entity.GetNativeControllerPawn();
    auto ExpectedPawn = ExpectedNativePawn.Get();
    const bool bExpectedIdentityLive =
        ExpectedPawn &&
        ExpectedPawn == ExpectedNativePawnIdentity &&
        PlayerAIEntity::IsLivePawn(ExpectedPawn);

    auto MakeTerminal =
        [&](const char* Reason)
        {
            const auto PreviousState = GetState();

            // Pass only a serial-validated expected actor to teardown. The raw
            // address remains an identity marker, but must not be dereferenced
            // after its UObject slot has been recycled. DespawnEntity also
            // samples the controller's current pawn, so a live original and an
            // unsolicited replacement are both removed.
            EliminatedPawn = bExpectedIdentityLive
                ? ExpectedPawn : CurrentPawn;
            SetTransitionDamageProtection(false);
            PlayerAINormalizeTerminalPawnDamage(ExpectedPawn);
            if (CurrentPawn != ExpectedPawn)
                PlayerAINormalizeTerminalPawnDamage(CurrentPawn);
            bDeathHandled = true;
            SetState(EPlayerAIState::Dead, Reason);
            AIDebugLogger::Error(
                "Lifecycle",
                "%s terminal native pawn lifecycle expected=%p current=%p generation=%d phase=%d state=%s reason=%s",
                Entity.DisplayName.c_str(),
                (void*)ExpectedNativePawnIdentity,
                (void*)CurrentPawn,
                NativePawnGeneration,
                (int)World.Phase,
                PlayerAIStateToString(PreviousState),
                Reason);
            EliminationBehavior::OnEliminated(*this, Now);
            return false;
        };

    if (!bNativePawnIdentityEstablished)
    {
        // SpawnNativeEntity normally supplies the pawn synchronously. Retain a
        // very small initial-assignment grace for builds which publish the
        // controller's Pawn property on the following server tick.
        const bool bInitialAssignmentWindow =
            NativePawnGeneration == 0 &&
            Now >= SpawnedAtTime &&
            Now - SpawnedAtTime <= 2.f &&
            World.Phase <= EPlayerAIMatchPhase::PreMatch;
        if (!CurrentPawn)
        {
            if (bInitialAssignmentWindow)
                return true;

            return MakeTerminal("native pawn was never assigned");
        }

        if (!bInitialAssignmentWindow)
            return MakeTerminal("late native pawn assignment rejected");

        if (bExpectedIdentityLive && CurrentPawn != ExpectedPawn)
        {
            return MakeTerminal(
                "conflicting initial native pawn assignment");
        }

        ExpectedNativePawn =
            TWeakObjectPtr<AFortPlayerPawnAthena>(CurrentPawn);
        ExpectedNativePawnIdentity = CurrentPawn;
        Entity.NativePawn = CurrentPawn;
        NativePawnGeneration = 1;
        bNativePawnIdentityEstablished = true;
        NativePawnHandoffMissingSince = -1.f;
        return true;
    }

    // Observe the expected actor directly, even if the native controller has
    // already detached from or replaced it. This closes the one-tick window in
    // which native respawn could otherwise hide a terminal health transition.
    if (bExpectedIdentityLive &&
        World.Phase == EPlayerAIMatchPhase::InProgress)
    {
        const float Health = ExpectedPawn->GetHealth();
        if (std::isfinite(Health) && Health <= 0.f &&
            !ExpectedPawn->IsDBNO())
        {
            return MakeTerminal("native pawn reached terminal health");
        }
    }

    const bool bExpectedPawnMatches =
        CurrentPawn &&
        CurrentPawn == ExpectedPawn &&
        CurrentPawn == ExpectedNativePawnIdentity;
    if (bExpectedPawnMatches)
    {
        NativePawnHandoffMissingSince = -1.f;

        if (bJumpedFromTransport ||
            (World.Phase == EPlayerAIMatchPhase::InProgress &&
             PlayerAIIsGroundState(GetState())))
        {
            bNativePawnIdentityLocked = true;
        }

        return true;
    }

    if (!CurrentPawn)
    {
        // A brief pre-jump detach is the only supported pawnless handoff. Once
        // the bot jumped (or reached normal play), a missing expected pawn is
        // terminal rather than a reason to let native respawn code recover it.
        if (PlayerAIIsNativeAircraftHandoffWindow(*this, World))
        {
            if (NativePawnHandoffMissingSince < 0.f ||
                Now < NativePawnHandoffMissingSince)
            {
                NativePawnHandoffMissingSince = Now;
            }

            if (Now - NativePawnHandoffMissingSince <=
                PlayerAINativePawnHandoffGraceSeconds)
                return true;
        }
    }
    else if (!bExpectedIdentityLive &&
        (NativePawnHandoffMissingSince < 0.f ||
         Now < NativePawnHandoffMissingSince ||
         Now - NativePawnHandoffMissingSince <=
             PlayerAINativePawnHandoffGraceSeconds) &&
        PlayerAIIsNativeAircraftHandoffWindow(*this, World))
    {
        // Some builds replace the parked warmup pawn before the virtual
        // passenger jumps. Adopt exactly one such pre-jump handoff, and never
        // when the old pawn is still live (which would be a duplicate).
        ExpectedNativePawn =
            TWeakObjectPtr<AFortPlayerPawnAthena>(CurrentPawn);
        ExpectedNativePawnIdentity = CurrentPawn;
        Entity.NativePawn = CurrentPawn;
        NativePawnGeneration++;
        bNativeAircraftHandoffConsumed = true;
        NativePawnHandoffMissingSince = -1.f;
        AIDebugLogger::Log(
            "Lifecycle",
            "%s accepted one native aircraft pawn handoff (generation %d)",
            Entity.DisplayName.c_str(), NativePawnGeneration);
        return true;
    }

    return MakeTerminal(
        CurrentPawn
            ? "unauthorized native pawn replacement"
            : "native pawn disappeared");
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

    if (GetState() == EPlayerAIState::Dead ||
        GetState() == EPlayerAIState::MatchEnded)
        return;

    if (!ValidateNativePawnIdentity(Now, World))
        return;

    // Match end freezes everyone.
    if (World.Phase == EPlayerAIMatchPhase::Ended)
    {
        NavigationBehavior::ClearMoveTarget(*this);
        SetTransitionDamageProtection(false);
        SetState(EPlayerAIState::MatchEnded, "match ended");
        return;
    }

    // Observe phase changes immediately, but spread transport setup over a
    // short window. Running landing selection/reflection for a full lobby in
    // one TickFlush caused a visible server freeze at bus start.
    if (World.Phase != ObservedPhase)
    {
        if (World.Phase == EPlayerAIMatchPhase::Transport &&
            !bJumpedFromTransport)
        {
            bTransportSetupPending = true;
        }
        else if (World.Phase == EPlayerAIMatchPhase::InProgress &&
            !bJumpedFromTransport &&
            (GetState() == EPlayerAIState::PreMatchIdle ||
             GetState() == EPlayerAIState::PreMatchWalking ||
             GetState() == EPlayerAIState::PreMatchEmoting ||
             GetState() == EPlayerAIState::WaitingForTransport))
        {
            // The transport phase was skipped on this playlist/version.
            bTransportSetupPending = true;
        }

        if (bTransportSetupPending)
        {
            const int Slot =
                AIIndex > 0 ? (AIIndex - 1) % 100 : 0;
            TransportSetupReadyTime =
                Now + Slot * 0.005f;
        }

        if (World.Phase == EPlayerAIMatchPhase::InProgress &&
            !bTransportSetupPending &&
            !bJumpedFromTransport &&
            (GetState() == EPlayerAIState::InTransport ||
             GetState() == EPlayerAIState::ChoosingLandingSpot ||
             GetState() == EPlayerAIState::WaitingForTransport))
        {
            ForcedJumpTime = Now;
            EarliestJumpTime = Now;
        }

        ObservedPhase = World.Phase;
    }

    if (bTransportSetupPending)
    {
        if (bJumpedFromTransport)
        {
            bTransportSetupPending = false;
        }
        else if (Now >= TransportSetupReadyTime)
        {
            TransportBehavior::OnTransportPhaseStarted(
                *this, Now, World);
            bTransportSetupPending = false;

            if (World.Phase ==
                EPlayerAIMatchPhase::InProgress)
            {
                ForcedJumpTime = Now;
                EarliestJumpTime = Now;
            }
        }
    }

    auto ActivePawn = GetPawn();

    if (ActivePawn)
    {
        NextPawnRecoveryTime = 0.f;
        PawnRecoveryAttempts = 0;
    }

    // A simulated pawn can be removed by map warmup teardown on unusual
    // playlists. Recover it only while still in warmup. During Transport a
    // pawnless controller is a valid virtual passenger: spawning it here
    // bypasses the locked-bus gate and used to create two airborne pawns per
    // frame until a full lobby stalled the game thread.
    if (!ActivePawn && !Entity.bNativeBacked &&
        World.Phase < EPlayerAIMatchPhase::Transport)
    {
        if (!PlayerAIManager::TryBeginPawnRecovery(
                *this, Now))
            return;

        FVector Recovery = bHasLandingTarget
            ? LandingTarget
            : HomeLocation;

        ActivePawn =
            MagnesiumPlayerAISpawner::SpawnPawnAt(
                *this, Recovery, true);

        if (!ActivePawn)
        {
            PlayerAIManager::FinishPawnRecovery(
                *this, false);
            return;
        }

        PlayerAIManager::FinishPawnRecovery(
            *this, true);
    }

    const auto State = GetState();
    const bool bTransitionProtected =
        World.Phase <= EPlayerAIMatchPhase::PreMatch ||
        State == EPlayerAIState::WaitingForTransport ||
        State == EPlayerAIState::InTransport ||
        State == EPlayerAIState::ChoosingLandingSpot ||
        State == EPlayerAIState::JumpingFromTransport ||
        State == EPlayerAIState::Gliding ||
        State == EPlayerAIState::Landing;

    // Pregame island damage and fall damage vary wildly by version. Keep the
    // pawn protected only while the match transition owns its position; the
    // first grounded combat/loot state restores normal damage immediately.
    SetTransitionDamageProtection(bTransitionProtected);

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
    case EPlayerAIState::Landing:
        NavigationBehavior::StepAirMovement(
            *this, Now, DeltaSeconds);
        break;

    default:
        if (PlayerAIIsMovingState(GetState()) && bHasMoveTarget)
            NavigationBehavior::StepMovement(*this, Now, DeltaSeconds);
        break;
    }

    // Simulated backend: keep idle pawns planted on the ground.
    if (!Entity.bNativeBacked &&
        (PlayerAIIsGroundState(GetState()) || GetState() == EPlayerAIState::PreMatchIdle ||
         GetState() == EPlayerAIState::PreMatchEmoting || GetState() == EPlayerAIState::PreMatchWalking))
        NavigationBehavior::SettleIdle(*this, Now, DeltaSeconds);

    // Void rescue: anyone who somehow fell below the world gets put back on
    // a sane anchor instead of falling forever / dying silently.
    if (PlayerAIIsGroundState(GetState()))
    {
        auto RescuePawn = GetPawn();

        if (RescuePawn)
        {
            FVector RescueLoc = RescuePawn->K2_GetActorLocation();

            if (RescueLoc.Z < -7000.0)
            {
                FVector Anchor = bHasLandingTarget ? LandingTarget : HomeLocation;
                const bool bGroundedRescue =
                    MagnesiumPlayerAISpawner::SpawnPawnAt(
                        *this, Anchor, true) != nullptr;
                bool bAirborneRescue = false;

                if (!bGroundedRescue)
                {
                    RescuePawn =
                        MagnesiumPlayerAISpawner::SpawnAirborneForLanding(
                            *this, Anchor);
                    bAirborneRescue = RescuePawn != nullptr;

                    if (bAirborneRescue)
                    {
                        SetState(
                            EPlayerAIState::Landing,
                            "below-world rescue awaiting ground");
                    }
                }

                NavigationBehavior::ClearMoveTarget(*this);
                PosVertVel = 0.f;
                bPosGrounded = false; // swept gravity re-verifies footing

                if (bGroundedRescue || bAirborneRescue)
                {
                    AIDebugLogger::Log(
                        "Navigation",
                        "%s rescued from below the world (%s)",
                        Entity.DisplayName.c_str(),
                        bGroundedRescue ? "grounded" : "airborne");
                }

                if (!bGroundedRescue)
                {
                    SetTransitionDamageProtection(true);

                    if (!bAirborneRescue)
                    {
                        SetState(
                            EPlayerAIState::Landing,
                            "below-world rescue pending");
                    }

                    return;
                }
            }
        }
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

static float PlayerAINextPlayersLeftRepairTime = 0.f;
static int PlayerAILastRosterCount = -1;
static int PlayerAIStablePlayersLeftMismatchTicks = 0;

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
    PlayerAINextPlayersLeftRepairTime = 0.f;
    PlayerAILastRosterCount = -1;
    PlayerAIStablePlayersLeftMismatchTicks = 0;
    PlayerAIResetLootDiscovery(true);

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

    auto CurrentWorld = UWorld::GetWorld();

    // With no managed AI there is no consumer for loot data. Abandon any
    // partial pass instead of spending server ticks scanning an empty lobby.
    if (Controllers.empty())
    {
        if (bPlayerAILootScanActive ||
            !World.Containers.empty() ||
            !World.Pickups.empty())
        {
            PlayerAIResetLootDiscovery(true);
        }

        return;
    }

    if (PlayerAILootScanWorld != CurrentWorld)
    {
        PlayerAIResetLootDiscovery(true);
        PlayerAILootScanWorld = CurrentWorld;
    }

    if (!CurrentWorld)
        return;

    if (!bPlayerAILootScanActive)
    {
        if (Now - World.LastLootScanTime <
            PlayerAIConfig::WorldSnapshotInterval)
        {
            return;
        }

        PlayerAILootScanCursor = 0;
        PlayerAILootScanLimit = TUObjectArray::Num();
        PlayerAIPendingContainers.clear();
        PlayerAIPendingPickups.clear();
        bPlayerAILootScanActive =
            PlayerAILootScanLimit > 0;

        if (!bPlayerAILootScanActive)
        {
            World.LastLootScanTime = Now;
            return;
        }
    }

    const UClass* ContainerClass =
        ABuildingContainer::StaticClass();
    const UClass* PickupClass =
        AFortPickupAthena::StaticClass();

    if (!ContainerClass && !PickupClass)
    {
        bPlayerAILootScanActive = false;
        World.LastLootScanTime = Now;
        return;
    }

    const int CurrentObjectCount = TUObjectArray::Num();
    const int End = (std::min)(
        PlayerAILootScanCursor +
            PlayerAILootDiscoveryBudgetPerTick,
        PlayerAILootScanLimit);

    for (int ObjectIndex = PlayerAILootScanCursor;
         ObjectIndex < End &&
         ObjectIndex < CurrentObjectCount;
         ObjectIndex++)
    {
        auto Object =
            TUObjectArray::GetObjectByIndex(ObjectIndex);

        if (!PlayerAIIsLiveObject(Object) ||
            Object->IsDefaultObject())
        {
            continue;
        }

        const bool bIsPickup =
            PickupClass &&
            Object->IsA(PickupClass);
        const bool bIsContainer =
            !bIsPickup &&
            ContainerClass &&
            Object->IsA(ContainerClass);

        if ((!bIsPickup && !bIsContainer) ||
            !PlayerAIObjectBelongsToWorld(
                Object, CurrentWorld))
        {
            continue;
        }

        if (bIsPickup)
        {
            PlayerAIPendingPickups.push_back(
                (AFortPickupAthena*)Object);
        }
        else
        {
            PlayerAIPendingContainers.push_back(
                (ABuildingContainer*)Object);
        }
    }

    PlayerAILootScanCursor = End;

    if (PlayerAILootScanCursor >=
        PlayerAILootScanLimit)
    {
        World.Containers.swap(
            PlayerAIPendingContainers);
        World.Pickups.swap(
            PlayerAIPendingPickups);
        PlayerAIPendingContainers.clear();
        PlayerAIPendingPickups.clear();
        PlayerAILootScanCursor = 0;
        PlayerAILootScanLimit = 0;
        bPlayerAILootScanActive = false;
        World.LastLootScanTime = Now;
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

bool PlayerAIManager::TryBeginPawnRecovery(
    PlayerAIController& AI,
    float Now)
{
    if (AI.PawnRecoveryAttempts >=
        PlayerAIMaxPawnRecoveryAttempts)
    {
        // Non-destructive quarantine: keep the controller/player-state alive
        // for native possession to recover, but never issue another spawn
        // during this missing-pawn episode.
        AI.NextPawnRecoveryTime = FLT_MAX;
        return false;
    }

    if (AI.NextPawnRecoveryTime <= 0.f)
    {
        const int Slot =
            AI.AIIndex > 0
            ? (AI.AIIndex - 1) % 100
            : 0;
        AI.NextPawnRecoveryTime =
            Now + Slot * 0.005f;
        return false;
    }

    if (Now < AI.NextPawnRecoveryTime ||
        !PlayerAITryConsumePawnRecoveryBudget())
    {
        return false;
    }

    AI.PawnRecoveryAttempts++;
    const int BackoffShift =
        (std::min)(
            AI.PawnRecoveryAttempts - 1, 5);
    const float RetryDelay =
        (std::min)(
            1.f * (float)(1 << BackoffShift),
            30.f);
    AI.NextPawnRecoveryTime = Now + RetryDelay;
    return true;
}

void PlayerAIManager::FinishPawnRecovery(
    PlayerAIController& AI,
    bool bSucceeded)
{
    if (bSucceeded)
    {
        AI.NextPawnRecoveryTime = 0.f;
        AI.PawnRecoveryAttempts = 0;
        return;
    }

    if (AI.PawnRecoveryAttempts <
        PlayerAIMaxPawnRecoveryAttempts)
    {
        return;
    }

    AI.NextPawnRecoveryTime = FLT_MAX;
    AIDebugLogger::Error(
        "Spawner",
        "%s pawn recovery failed %d times - spawn recovery quarantined",
        AI.Entity.DisplayName.c_str(),
        AI.PawnRecoveryAttempts);
}

bool PlayerAIManager::TryBeginTransportJump()
{
    if (PlayerAITransportJumpBudgetRemaining <= 0)
        return false;

    PlayerAITransportJumpBudgetRemaining--;
    return true;
}

void PlayerAIManager::UpdateAll(float Now, float DeltaSeconds)
{
    if (!bInitialized)
        return;

    if (Controllers.empty())
    {
        if (PlayerAILootScanWorld ||
            bPlayerAILootScanActive ||
            !World.Containers.empty() ||
            !World.Pickups.empty())
        {
            PlayerAIResetLootDiscovery(true);
        }

        return;
    }

    PlayerAIPawnRecoveryBudgetRemaining =
        PlayerAIPawnRecoveryBudgetPerTick;
    PlayerAITransportJumpBudgetRemaining =
        PlayerAITransportJumpBudgetPerTick;
    RefreshWorldSnapshot(Now);

    std::vector<int> CleanupIndices;

    for (int ControllerIndex = 0;
         ControllerIndex < (int)Controllers.size();
         ControllerIndex++)
    {
        auto& Controller = Controllers[ControllerIndex];

        if (!Controller)
            continue;

        // Death is terminal for PlayerAI even when the current playlist has
        // player respawns enabled. The native death callback can replace the
        // pawn before this tick; DespawnEntity deliberately tears down both
        // that replacement and the remembered eliminated pawn.
        if (Controller->bDeathHandled ||
            Controller->GetState() == EPlayerAIState::Dead)
        {
            CleanupIndices.push_back(ControllerIndex);
            continue;
        }

        if (!Controller->Entity.IsValid())
        {
            AIDebugLogger::Error(
                "Manager",
                "AIPlayer '%s' lost its engine entity - removing stale roster entry",
                Controller->Entity.DisplayName.c_str());
            CleanupIndices.push_back(ControllerIndex);
            continue;
        }

        if (PlayerAITryUpdate(Controller.get(), Now, DeltaSeconds, World))
        {
            // Only consecutive faults quarantine an AI. Three unrelated
            // guarded feature misses over a whole match must not strand a
            // live roster entry.
            Controller->FaultCount = 0;
            continue;
        }

        Controller->FaultCount++;
        AIDebugLogger::Error("Manager", "AIPlayer '%s' update faulted (%d/3) - contained",
            Controller->Entity.DisplayName.c_str(), Controller->FaultCount);

        if (Controller->FaultCount >= 3)
        {
            // Remove a repeatedly faulting entity completely. Leaving a
            // frozen controller in AlivePlayers/AliveBots creates an
            // unkillable roster ghost and prevents the match from ending.
            Controller->SetTransitionDamageProtection(false);
            Controller->bDeathHandled = true;
            Controller->SetState(EPlayerAIState::MatchEnded, "quarantined after repeated faults");
            AIDebugLogger::Error("Manager", "AIPlayer '%s' quarantined to protect the gameserver",
                Controller->Entity.DisplayName.c_str());
            CleanupIndices.push_back(ControllerIndex);
        }
    }

    for (int CleanupIndex = (int)CleanupIndices.size() - 1;
         CleanupIndex >= 0; CleanupIndex--)
    {
        const int ControllerIndex = CleanupIndices[CleanupIndex];
        auto& Controller = Controllers[ControllerIndex];

        if (Controller)
        {
            Controller->SetTransitionDamageProtection(false);
            MagnesiumPlayerAISpawner::DespawnEntity(
                *Controller,
                "invalid or quarantined PlayerAI",
                false);
        }

        Controllers.erase(Controllers.begin() + ControllerIndex);
    }

    if (!CleanupIndices.empty())
        VersionFeatureAdapter::SyncPlayersLeft(true);

    // Repair native death/spawn ordering without fighting a one-frame engine
    // transition. Once the unique AlivePlayers + AliveBots roster disagrees
    // for two consecutive samples, publish it and explicitly dirty the
    // push-model PlayersLeft property used by modern minimap HUDs.
    if (Now >= PlayerAINextPlayersLeftRepairTime)
    {
        PlayerAINextPlayersLeftRepairTime = Now + 0.5f;
        VersionFeatureAdapter::
            RetryPendingPlayersLeftReplication();

        const int RosterCount =
            VersionFeatureAdapter::CountAliveParticipants();
        const int ReplicatedCount =
            World.GameState ? World.GameState->PlayersLeft : RosterCount;

        if (World.GameState && ReplicatedCount != RosterCount)
        {
            if (RosterCount == PlayerAILastRosterCount)
                PlayerAIStablePlayersLeftMismatchTicks++;
            else
                PlayerAIStablePlayersLeftMismatchTicks = 1;

            if (PlayerAIStablePlayersLeftMismatchTicks >= 2)
            {
                VersionFeatureAdapter::ReplicatePlayersLeft(
                    World.GameState, RosterCount, true);
                PlayerAIStablePlayersLeftMismatchTicks = 0;
            }
        }
        else
        {
            PlayerAIStablePlayersLeftMismatchTicks = 0;
        }

        PlayerAILastRosterCount = RosterCount;
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
    if (Controller->Entity.bNativeBacked)
    {
        auto ControllerNativePawn =
            Controller->Entity.GetNativeControllerPawn();
        auto InitialNativePawn = ControllerNativePawn;
        if (!InitialNativePawn &&
            PlayerAIEntity::IsLivePawn(
                Controller->Entity.NativePawn))
        {
            InitialNativePawn =
                Controller->Entity.NativePawn;
        }

        if (InitialNativePawn)
        {
            Controller->ExpectedNativePawn =
                TWeakObjectPtr<AFortPlayerPawnAthena>(
                    InitialNativePawn);
            Controller->ExpectedNativePawnIdentity =
                InitialNativePawn;
            Controller->Entity.NativePawn =
                InitialNativePawn;

            // A returned spawn pawn is useful for bounded teardown, but only
            // the controller's possession property establishes generation 1.
            // Some builds publish that property on the next server tick.
            if (ControllerNativePawn)
            {
                Controller->NativePawnGeneration = 1;
                Controller->bNativePawnIdentityEstablished =
                    true;
            }
        }
    }
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
    Controller->SetTransitionDamageProtection(true);
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
    const bool bRemovedAny = !Controllers.empty();

    for (auto& Controller : Controllers)
    {
        if (Controller)
            MagnesiumPlayerAISpawner::DespawnEntity(
                *Controller, Reason, false);
    }

    Controllers.clear();

    if (bRemovedAny)
        VersionFeatureAdapter::SyncPlayersLeft(true);

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

bool PlayerAIManager::HandleControllerDeath(
    AFortPlayerControllerAthena* PC,
    AFortPlayerPawnAthena* EliminatedPawn)
{
    auto AI = FindByController(PC);

    if (!AI)
        return false;

    if (EliminatedPawn)
        AI->EliminatedPawn = EliminatedPawn;

    if (AI->bDeathHandled)
        return true;

    AI->SetTransitionDamageProtection(false);
    AI->bDeathHandled = true;
    AI->SetState(EPlayerAIState::Dead, "native pawn death");
    EliminationBehavior::OnEliminated(
        *AI, VersionFeatureAdapter::GetTimeSeconds());
    return true;
}

PlayerAIController* PlayerAIManager::FindByController(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return nullptr;

    // Pointer comparison only - PC may be a real player controller, a
    // simulated PlayerAI controller or a native bot controller.
    for (auto& Controller : Controllers)
    {
        if (!Controller)
            continue;

        if (Controller->Entity.PC == PC || Controller->Entity.NativeController == (AActor*)PC)
            return Controller.get();
    }

    return nullptr;
}

bool PlayerAIManager::IsLiveWorldActor(
    const AActor* Actor,
    const UClass* ExpectedClass)
{
    auto CurrentWorld = UWorld::GetWorld();

    // Membership was verified when the staged cache was built. At use time
    // only reject a world swap and validate the object-array slot/flags; do
    // not repeat an outer-chain walk for every loot candidate of every AI.
    return CurrentWorld &&
        PlayerAILootScanWorld == CurrentWorld &&
        ExpectedClass &&
        PlayerAIIsLiveObject(Actor) &&
        Actor->IsA(ExpectedClass);
}
