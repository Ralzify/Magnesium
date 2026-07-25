#include "pch.h"
// ============================================================================
// Magnesium PlayerAI - behavior modules
//   PlayerAIStateMachine, NavigationBehavior, PreMatchBehavior, EmoteBehavior,
//   TransportBehavior, LandingBehavior, LootingBehavior, InventoryBehavior,
//   ZoneRotationBehavior, CombatBehavior, DamageBehavior, EliminationBehavior,
//   VictoryConditionBehavior, ReplicationBehavior
// ============================================================================
#include "../Public/PlayerAIStateMachine.h"
#include "../Public/PlayerAIController.h"
#include "../Public/PlayerAIManager.h"
#include "../Public/PlayerAIConfig.h"
#include "../Public/AIDebugLogger.h"
#include "../Public/VersionFeatureAdapter.h"
#include "../Public/NavigationBehavior.h"
#include "../Public/PreMatchBehavior.h"
#include "../Public/EmoteBehavior.h"
#include "../Public/TransportBehavior.h"
#include "../Public/LandingBehavior.h"
#include "../Public/LootingBehavior.h"
#include "../Public/InventoryBehavior.h"
#include "../Public/ZoneRotationBehavior.h"
#include "../Public/CombatBehavior.h"
#include "../Public/DamageBehavior.h"
#include "../Public/EliminationBehavior.h"
#include "../Public/VictoryConditionBehavior.h"
#include "../Public/ReplicationBehavior.h"
#include "../Public/MagnesiumPlayerAISpawner.h"
#include "../Public/NativePlayerAIBackend.h"
#include "../Public/PlayerAIFaultGuard.h"
#include "../../../FortniteGame/Public/FortLootPackage.h"
#include "../../../FortniteGame/Public/FortWeaponMods.h"
#include <unordered_map>
#include <algorithm>

static constexpr double PLAYERAI_RAD_TO_DEG = 57.29577951308232;
static constexpr float PLAYERAI_PAWN_HALF_HEIGHT = 90.f;

// ---- Simulated-tier runtime capability probes -------------------------------
// The engine's own character movement usually simulates connectionless
// player pawns server side (gravity, collision, real walking) - which is
// exactly what "walking normally" needs on every version. Probed at runtime;
// only when a version provably does not move pawns from input does the old
// position-driven fallback take over.
static int GSimMovementMode = -1;  // -1 probing, 1 input movement works, 0 position fallback
static int GSimMoveProbeFails = 0;

// Whether PawnStartFire produces real shots (ammo is consumed). When real
// fire works, simulated damage switches off - the engine's bullets carry
// the damage, sound and kill credit.
static int GRealFireWorks = -1;    // -1 probing, 1 real bullets, 0 cosmetic only
static int GRealFireProbes = 0;

// When trigger fire is dead, shots go through the weapon's primary fire
// ability instead (server-activated): real bullets with sound/tracers and
// native damage where the ability path works on a version.
static int GAbilityFireWorks = -1; // -1 probing, 1 real bullets, 0 cosmetic only
static int GAbilityFireProbes = 0;

// Drop/air states: no combat in either direction while skydiving/gliding.
static bool PlayerAIIsAirState(EPlayerAIState State)
{
    return State == EPlayerAIState::ChoosingLandingSpot ||
           State == EPlayerAIState::WaitingForTransport ||
           State == EPlayerAIState::InTransport ||
           State == EPlayerAIState::JumpingFromTransport ||
           State == EPlayerAIState::Gliding;
}

static int PlayerAISprintStyleValue()
{
    static int Cached = -2;

    if (Cached == -2)
    {
        auto Enum = FindEnum("EFortMovementStyle");
        Cached = Enum ? (int)Enum->GetValue("Sprinting") : -1;
    }

    return Cached;
}

// ============================================================================
// PlayerAIStateMachine
// ============================================================================

void PlayerAIStateMachine::Transition(EPlayerAIState NewState, float Now, const char* OwnerName, const char* Reason)
{
    if (State == NewState)
        return;

    AIDebugLogger::Verbose("State", "%s: %s -> %s (%s)", OwnerName ? OwnerName : "AIPlayer",
        PlayerAIStateToString(State), PlayerAIStateToString(NewState), Reason ? Reason : "");

    State = NewState;
    StateEnterTime = Now;
}

// ============================================================================
// ReplicationBehavior
// ============================================================================

void ReplicationBehavior::SetupPawnReplication(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn)
        return;

    // Same replication setup Magnesium applies to server driven player
    // pawns: always relevant, never dormant, high update frequency. The
    // server stays authoritative; clients receive the AI like any other
    // player-like entity.
    // TODO: connect this to the Magnesium movement replication system for
    //       per-version interpolation tuning if desired.
    Pawn->bReplicates = true;
    Pawn->bAlwaysRelevant = true;
    Pawn->NetCullDistanceSquared = FLT_MAX;
    Pawn->SetNetDormancy(ENetDormancy::DORM_Never);
    Pawn->NetUpdateFrequency = 100.f;
    Pawn->MinNetUpdateFrequency = 100.f;
}

void ReplicationBehavior::PushHealthShieldUpdate(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn)
        return;

    Pawn->ForceNetUpdate();
}

void ReplicationBehavior::PushTeleportUpdate(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn)
        return;

    if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
        Pawn->CharacterMovement->Velocity = FVector{};

    Pawn->ForceNetUpdate();
}

// ============================================================================
// NavigationBehavior
// ============================================================================

void NavigationBehavior::SetMoveTarget(PlayerAIController& AI, const FVector& Target)
{
    AI.MoveTarget = FVector(Target);
    AI.bHasMoveTarget = true;
}

void NavigationBehavior::ClearMoveTarget(PlayerAIController& AI)
{
    AI.bHasMoveTarget = false;

    if (AI.Entity.bNativeBacked)
    {
        NativePlayerAIBackend::StopMove(AI);
        return;
    }

    auto Pawn = AI.GetPawn();

    if (Pawn && Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
    {
        auto Vel = Pawn->CharacterMovement->Velocity;
        Pawn->CharacterMovement->Velocity = FVector(0, 0, Vel.Z);
    }
}

FVector NavigationBehavior::RandomPointNear(const FVector& Center, float Radius)
{
    const float Angle = PlayerAIRandRange(0.f, 6.2831853f);
    const float Dist = PlayerAIRandRange(Radius * 0.25f, Radius);

    FVector Point = Center;
    Point.X += cos(Angle) * Dist;
    Point.Y += sin(Angle) * Dist;

    bool bFound = false;
    FVector Ground = VersionFeatureAdapter::FindGroundLocation(Point, bFound);
    return bFound ? Ground : Point;
}

bool NavigationBehavior::StepMovement(PlayerAIController& AI, float Now, float DeltaSeconds, float SpeedOverride)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn || !AI.bHasMoveTarget)
        return true;

    FVector Loc = Pawn->K2_GetActorLocation();
    FVector To = AI.MoveTarget - Loc;
    To.Z = 0;

    const double Dist = To.Magnitude();

    if (Dist < 90.0)
    {
        ClearMoveTarget(AI);
        return true;
    }

    // Native backend: the engine's pathfinding + character movement do the
    // walking (navmesh paths, real slopes, stairs, no wall climbing, real
    // swimming) - no position writes at all.
    if (AI.Entity.bNativeBacked)
    {
        NativePlayerAIBackend::MoveTo(AI, AI.MoveTarget, 90.f, true);
        return false;
    }

    FVector Dir = To / Dist;

    float Speed = SpeedOverride > 0.f ? SpeedOverride : AI.Skill.MoveSpeed;
    // Movement quality: lower skill wobbles the speed a little more.
    Speed *= 0.90f + 0.20f * AI.Skill.MovementQuality;

    // ---- Simulated backend, input-driven walking (preferred) --------------
    // Feed the engine's own character movement instead of writing positions:
    // real gravity, collision, slopes and step-ups on every version - no
    // floating, no wall climbing, no flying.
    if (GSimMovementMode != 0)
    {
        // Water/void/ledge guard - only meaningful while ground tracing is
        // reliable on this version. Without trace data the native collision
        // handles walls and the void-rescue watchdogs handle the rest;
        // treating "no data" as "blocked" froze bots and stacked them into
        // the sky.
        if (VersionFeatureAdapter::IsGroundTraceReliable())
        {
            if (Now >= AI.NextGroundSnapTime)
            {
                AI.NextGroundSnapTime = Now + PlayerAIConfig::GroundSnapInterval;

                double SampleDist = Speed * 0.35;

                if (SampleDist < 150.0)
                    SampleDist = 150.0;

                FVector Ahead = Loc + Dir * SampleDist;
                bool bFound = false;
                FVector Ground = VersionFeatureAdapter::FindGroundLocation(Ahead, bFound, Pawn);

                AI.bGroundAheadValid = bFound;

                if (bFound)
                    AI.CachedGroundZ = (float)Ground.Z;
            }

            const bool bPreMatchState = AI.GetState() == EPlayerAIState::PreMatchIdle ||
                AI.GetState() == EPlayerAIState::PreMatchWalking ||
                AI.GetState() == EPlayerAIState::PreMatchEmoting;

            const double DropAhead = (double)Loc.Z - (double)AI.CachedGroundZ;

            if (!AI.bGroundAheadValid || (bPreMatchState && DropAhead > 600.0) || DropAhead > 4000.0)
            {
                // Nothing safe to walk onto: stop; the think/stuck logic
                // picks a different goal.
                ClearMoveTarget(AI);
                return true;
            }
        }
        else
        {
            AI.bGroundAheadValid = true;
        }

        Pawn->AddMovementInput(Dir, 1.f, true);

        FRotator FaceRot{};
        FaceRot.Yaw = atan2(Dir.Y, Dir.X) * PLAYERAI_RAD_TO_DEG;
        Pawn->K2_SetActorRotation(FaceRot, false);

        if (AI.Entity.PC)
            AI.Entity.PC->SetControlRotation(FaceRot);

        // Sprint style for faraway goals (visual only, safe where present).
        if (Pawn->HasCurrentMovementStyle())
        {
            const int Sprinting = PlayerAISprintStyleValue();

            if (Sprinting >= 0 && Dist > 1500.0 && Pawn->CurrentMovementStyle != (uint8)Sprinting)
                Pawn->CurrentMovementStyle = (uint8)Sprinting;
        }

        // Probe: does input actually move this pawn on this version?
        if (GSimMovementMode == -1)
        {
            if (AI.MoveProbeTime <= 0.f)
            {
                AI.MoveProbeTime = Now + 1.5f;
                AI.MoveProbeLoc = Loc;
            }
            else if (Now >= AI.MoveProbeTime)
            {
                const double Moved = Loc.GetDistanceTo(AI.MoveProbeLoc);

                if (Moved > 100.0)
                {
                    GSimMovementMode = 1;
                    AIDebugLogger::Log("Navigation", "native character movement drives PlayerAI on this version (input mode)");
                }
                else if (++GSimMoveProbeFails >= 3)
                {
                    // Swept walking needs no ground traces, so a failed
                    // input probe always falls back to it.
                    GSimMovementMode = 0;
                    AIDebugLogger::MissingFeature("InputMovementForPlayerAI",
                        "input does not move pawns on this version - using engine-swept walking");
                }

                AI.MoveProbeTime = 0.f;
            }
        }

        return false;
    }

    // ---- Position fallback: engine-swept walking ---------------------------
    // K2_SetActorLocation with bSweep moves the pawn's real collision
    // capsule: walls and floors block it, stairs step up, and non-blocking
    // meshes (tree canopies, foliage) are passed straight through. Gravity
    // is a swept drop every step, so the pawn always rests on actual
    // geometry - the same collision real player movement uses. No ground
    // traces, no terrain guessing, nothing that can walk a pawn into the sky.
    double MoveStep = Speed * DeltaSeconds;

    if (MoveStep > 400.0)
        MoveStep = 400.0; // hitch guard: never leap on a long frame

    const double StepUp = 55.0;    // stair/kerb allowance while grounded
    const double StepDown = 140.0; // slope follow-down allowance while grounded

    FVector Want = Loc;
    Want.X = Loc.X + Dir.X * MoveStep;
    Want.Y = Loc.Y + Dir.Y * MoveStep;

    if (AI.bPosGrounded)
        Want.Z = Loc.Z + StepUp;

    Pawn->K2_SetActorLocation(Want, true, nullptr, false);

    FVector Mid = Pawn->K2_GetActorLocation();

    // Vertical: integrate gravity; TryJump sets an upward velocity first.
    AI.PosVertVel -= 2200.f * DeltaSeconds;

    if (AI.PosVertVel < -3800.f)
        AI.PosVertVel = -3800.f;

    double VertDelta = (double)AI.PosVertVel * DeltaSeconds;

    if (AI.bPosGrounded)
        VertDelta -= StepUp + StepDown; // undo the lift, follow slopes down

    FVector VertTo = Mid;
    VertTo.Z = Mid.Z + VertDelta;

    Pawn->K2_SetActorLocation(VertTo, true, nullptr, false);

    FVector Final = Pawn->K2_GetActorLocation();

    if (VertDelta < 0.0)
    {
        // A blocked drop means standing on real geometry; a completed one
        // means airborne (ledge, jump apex) - keep falling next step.
        const bool bOnGround = Final.Z > VertTo.Z + 1.0;

        if (bOnGround)
            AI.PosVertVel = 0.f;

        AI.bPosGrounded = bOnGround;
    }
    else if (Final.Z < VertTo.Z - 1.0)
    {
        AI.PosVertVel = 0.f; // jump bumped a ceiling: start falling
    }

    FRotator FaceRot{};
    FaceRot.Yaw = atan2(Dir.Y, Dir.X) * PLAYERAI_RAD_TO_DEG;
    Pawn->K2_SetActorRotation(FaceRot, false);

    // Velocity is replicated and drives movement animations on clients.
    if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
    {
        FVector Vel = Dir * Speed;
        Vel.Z = AI.PosVertVel;
        Pawn->CharacterMovement->Velocity = Vel;
    }

    return false;
}

void NavigationBehavior::StepAirMovement(PlayerAIController& AI, float DeltaSeconds)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn || !AI.bHasLandingTarget)
        return;

    FVector Loc = Pawn->K2_GetActorLocation();
    FVector To = AI.LandingTarget - Loc;
    To.Z = 0;

    const double Dist = To.Magnitude();
    FVector Dir = Dist > 1.0 ? To / Dist : FVector(1, 0, 0);

    // Every Gliding pawn is engine-managed now (native aircraft jump or the
    // native backend's skydive): the engine handles descent, collision and
    // landing - we only steer horizontally with input and brake edge cases.
    // (All manual descent/position writing is gone: it tunneled pawns
    // through terrain and fought the native glider.)
    if (Dist > 300.0)
        Pawn->AddMovementInput(Dir, 1.f, true);

    if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
    {
        auto Vel = Pawn->CharacterMovement->Velocity;
        const double AnchorZ = (double)AI.LandingTarget.Z;
        bool bAdjust = false;

        // Brake lethal free-falls near the ground (no glider deployed).
        if (Vel.Z < -3200.0 && (Loc.Z - AnchorZ) < 4000.0)
        {
            Vel.Z = -1200.0;
            bAdjust = true;
        }
        // Push barely-descending skydivers down (an input-less skydive can
        // hang almost still - AI floating in the sky).
        else if (Vel.Z > -400.0 && (Loc.Z - AnchorZ) > 1200.0)
        {
            Vel.Z = -1000.0;
            bAdjust = true;
        }

        if (bAdjust)
            Pawn->CharacterMovement->Velocity = Vel;
    }

    FRotator SteerRot{};
    SteerRot.Yaw = atan2(Dir.Y, Dir.X) * PLAYERAI_RAD_TO_DEG;
    Pawn->K2_SetActorRotation(SteerRot, false);
}

void NavigationBehavior::TryJump(PlayerAIController& AI, float Now)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    // Native backend: a real jump through the possessed pawn.
    if (AI.Entity.bNativeBacked)
    {
        NativePlayerAIBackend::Jump(AI);
        return;
    }

    // Simulated backend with working input movement: real jump, released
    // shortly after (a held jump makes the pawn re-jump on every landing).
    if (GSimMovementMode != 0)
    {
        Pawn->Jump();
        AI.JumpStopTime = Now + 0.30f;
        return;
    }

    // Swept-walking fallback: a real parabolic hop over the same collision.
    if (AI.bPosGrounded)
    {
        AI.PosVertVel = 680.f;
        AI.bPosGrounded = false;
    }
}

void NavigationBehavior::SettleIdle(PlayerAIController& AI, float Now, float DeltaSeconds)
{
    // Idle gravity for swept-walking pawns (with working input movement the
    // native character movement keeps everyone grounded). Placements and
    // teleports can leave a pawn hovering - swept drops land it on real
    // collision, falling straight through non-blocking canopies and roofs
    // that fooled the old ground traces.
    if (AI.Entity.bNativeBacked || GSimMovementMode != 0 || AI.bHasMoveTarget)
        return;

    // Cheap while grounded (short probe a few times a second); per-tick
    // integration only while actually falling.
    if (AI.bPosGrounded)
    {
        if (Now < AI.NextGroundSnapTime)
            return;

        AI.NextGroundSnapTime = Now + 0.4f;
    }

    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    FVector Loc = Pawn->K2_GetActorLocation();

    AI.PosVertVel -= 2200.f * DeltaSeconds;

    if (AI.PosVertVel < -3800.f)
        AI.PosVertVel = -3800.f;

    double VertDelta = (double)AI.PosVertVel * DeltaSeconds;

    if (AI.bPosGrounded)
        VertDelta -= 40.0; // grounded probe depth

    FVector VertTo = Loc;
    VertTo.Z = Loc.Z + VertDelta;

    Pawn->K2_SetActorLocation(VertTo, true, nullptr, false);

    FVector Final = Pawn->K2_GetActorLocation();
    const bool bOnGround = Final.Z > VertTo.Z + 1.0;

    if (bOnGround)
        AI.PosVertVel = 0.f;

    AI.bPosGrounded = bOnGround;
}

void NavigationBehavior::CheckStuck(PlayerAIController& AI, float Now)
{
    if (!AI.bHasMoveTarget)
    {
        AI.StuckCounter = 0;
        return;
    }

    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    if (Now < AI.NextStuckCheckTime)
        return;

    FVector Loc = Pawn->K2_GetActorLocation();

    if (AI.NextStuckCheckTime > 0.f)
    {
        const double Moved = Loc.GetDistanceTo(AI.LastStuckCheckLocation);

        if (Moved < PlayerAIConfig::StuckDistanceThreshold)
        {
            AI.StuckCounter++;
            AIDebugLogger::Verbose("Navigation", "%s stuck (level %d)", AI.Entity.DisplayName.c_str(), AI.StuckCounter);

            if (AI.StuckCounter <= PlayerAIConfig::StuckRetriesBeforeNewTarget)
            {
                // Recovery 1: repath sideways + jump over small obstacles.
                FVector Side = AI.MoveTarget;
                Side.X += PlayerAIRandRange(-800.f, 800.f);
                Side.Y += PlayerAIRandRange(-800.f, 800.f);
                SetMoveTarget(AI, RandomPointNear(Side, 400.f));
                TryJump(AI, Now);
            }
            else if (AI.StuckCounter < PlayerAIConfig::StuckRetriesBeforeTeleport)
            {
                // Recovery 2: abandon the current target; the next think
                // picks a fresh one.
                AIDebugLogger::Log("Navigation", "%s abandoning unreachable target after being stuck", AI.Entity.DisplayName.c_str());
                ClearMoveTarget(AI);
                AI.LootContainerTarget = nullptr;
                AI.LootPickupTarget = nullptr;
            }
            else
            {
                // Recovery 3 (last resort): teleport to a valid point near
                // the target - ONLY onto traced ground and NEVER upward.
                // The trace happily reports tree canopies and prop tops as
                // "ground"; teleporting up there is what made AI pop into
                // the air and float around the treetops.
                bool bFound = false;
                FVector Ground = VersionFeatureAdapter::FindGroundLocation(AI.MoveTarget, bFound, Pawn);

                if (bFound && Ground.Z <= Loc.Z + 250.0)
                {
                    FVector SafePoint = Ground;
                    SafePoint.Z += 100.f;

                    AIDebugLogger::Log("Navigation", "%s teleport recovery to (%.0f, %.0f, %.0f) after repeated stuck detection",
                        AI.Entity.DisplayName.c_str(), SafePoint.X, SafePoint.Y, SafePoint.Z);

                    Pawn->K2_TeleportTo(SafePoint, Pawn->K2_GetActorRotation(), false, true);
                    ReplicationBehavior::PushTeleportUpdate(Pawn);
                    AI.PosVertVel = 0.f;
                    AI.bPosGrounded = false; // swept gravity re-verifies footing
                }
                else
                {
                    // Upward or unknown destination: abandon this goal and
                    // walk somewhere else instead of teleporting badly.
                    ClearMoveTarget(AI);
                    AI.LootContainerTarget = nullptr;
                    AI.LootPickupTarget = nullptr;
                }

                AI.StuckCounter = 0;
            }
        }
        else
        {
            AI.StuckCounter = 0;
        }
    }

    AI.LastStuckCheckLocation = Loc;
    AI.NextStuckCheckTime = Now + PlayerAIConfig::StuckCheckInterval;
}

// ============================================================================
// EmoteBehavior
// ============================================================================

bool EmoteBehavior::TryStartEmote(PlayerAIController& AI, float Now)
{
    if (!VersionFeatureAdapter::SupportsEmotes())
        return false; // unsupported: caller keeps walking/idling - no fallback animation

    auto Emote = VersionFeatureAdapter::GetRandomEmoteAsset();

    if (!Emote || !AI.Entity.PC)
        return false;

    VersionFeatureAdapter::PlayEmote(AI.Entity.PC, Emote);
    AI.EmoteEndTime = Now + PlayerAIConfig::PreMatchEmoteDuration;

    AIDebugLogger::Verbose("Emote", "%s started an emote", AI.Entity.DisplayName.c_str());
    return true;
}

void EmoteBehavior::StopEmote(PlayerAIController& AI)
{
    // Emotes end naturally when the AI starts moving again; nothing forced
    // here keeps this safe on every version.
    AI.EmoteEndTime = 0.f;
}

// ============================================================================
// PreMatchBehavior
// ============================================================================

// Crouch/uncrouch is flavor only: invoked with a sized zeroed parameter
// buffer inside a fault guard, disabled for the session if it faults.
static void PlayerAITryCrouchToggle(PlayerAIController& AI, AFortPlayerPawnAthena* Pawn)
{
    static bool bCrouchDisabled = false;

    if (bCrouchDisabled || !Pawn)
        return;

    auto CrouchFn = Pawn->GetFunction(PlayerAIRandChance(0.5f) ? "Crouch" : "UnCrouch");

    if (!CrouchFn)
        return;

    if (!VersionFeatureAdapter::SafeCallNoArgs(Pawn, CrouchFn))
    {
        bCrouchDisabled = true;
        AIDebugLogger::MissingFeature("CrouchForPlayerAI",
            "native crouch faulted and was disabled - PlayerAI skips crouching");
    }
}

void PreMatchBehavior::Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    switch (AI.GetState())
    {
    case EPlayerAIState::PreMatchIdle:
    {
        if (Now < AI.PreMatchActionEndTime)
        {
            // Idle look-around: turn left/right occasionally like a real
            // player scanning the island.
            if (PlayerAIRandChance(0.06f))
            {
                FRotator LookRot = Pawn->K2_GetActorRotation();
                LookRot.Yaw = LookRot.Yaw + PlayerAIRandRange(-130.f, 130.f);
                Pawn->K2_SetActorRotation(LookRot, false);

                if (AI.Entity.PC)
                    AI.Entity.PC->SetControlRotation(LookRot);
            }
            break;
        }

        // Never stand still for too long: pick the next natural action.
        if (PlayerAIRandChance(PlayerAIConfig::PreMatchEmoteChance) && EmoteBehavior::TryStartEmote(AI, Now))
        {
            AI.SetState(EPlayerAIState::PreMatchEmoting, "random emote");
            break;
        }

        // Occasional crouch toggle when the version supports it.
        if (PlayerAIRandChance(0.10f) && VersionFeatureAdapter::SupportsCrouch(Pawn))
            PlayerAITryCrouchToggle(AI, Pawn);

        NavigationBehavior::SetMoveTarget(AI, NavigationBehavior::RandomPointNear(AI.HomeLocation, PlayerAIConfig::PreMatchWanderRadius));
        AI.SetState(EPlayerAIState::PreMatchWalking, "wander");
        break;
    }

    case EPlayerAIState::PreMatchWalking:
    {
        if (!AI.bHasMoveTarget)
        {
            AI.PreMatchActionEndTime = Now + PlayerAIRandRange(PlayerAIConfig::PreMatchIdleTimeMin, PlayerAIConfig::PreMatchIdleTimeMax);
            AI.SetState(EPlayerAIState::PreMatchIdle, "arrived");
            break;
        }

        if (PlayerAIRandChance(PlayerAIConfig::PreMatchJumpChance))
            NavigationBehavior::TryJump(AI, Now);

        NavigationBehavior::CheckStuck(AI, Now);
        break;
    }

    case EPlayerAIState::PreMatchEmoting:
    {
        if (Now >= AI.EmoteEndTime)
        {
            EmoteBehavior::StopEmote(AI);
            AI.PreMatchActionEndTime = Now + PlayerAIRandRange(0.5f, 2.0f);
            AI.SetState(EPlayerAIState::PreMatchIdle, "emote finished");
        }
        break;
    }

    default:
        break;
    }
}

void PreMatchBehavior::Tick(PlayerAIController& AI, float Now, float DeltaSeconds)
{
    if (AI.GetState() == EPlayerAIState::PreMatchWalking)
        NavigationBehavior::StepMovement(AI, Now, DeltaSeconds, AI.Skill.MoveSpeed * 0.85f);

    // Island edge rescue: anyone who slips off the pre-match island gets
    // put back at their spawn point instead of dying in the void.
    auto Pawn = AI.GetPawn();

    if (Pawn)
    {
        FVector Loc = Pawn->K2_GetActorLocation();

        if (Loc.Z < AI.HomeLocation.Z - 3000.0)
        {
            FVector Back = AI.HomeLocation;
            Back.Z = Back.Z + 100.0;

            Pawn->K2_TeleportTo(Back, Pawn->K2_GetActorRotation(), false, true);
            ReplicationBehavior::PushTeleportUpdate(Pawn);
            NavigationBehavior::ClearMoveTarget(AI);
            AI.PosVertVel = 0.f;
            AI.bPosGrounded = false; // swept gravity re-verifies footing

            AIDebugLogger::Verbose("Navigation", "%s rescued from the island edge", AI.Entity.DisplayName.c_str());
        }
    }
}

// ============================================================================
// TransportBehavior
// ============================================================================

void TransportBehavior::OnTransportPhaseStarted(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    AI.TransportPhaseStartTime = Now;
    AI.ForcedJumpTime = Now + PlayerAIRandRange(8.f, PlayerAIConfig::ForcedJumpTimeAfterPhase);
    AI.ThankDriverTime = Now + PlayerAIRandRange(2.f, 10.f);
    AI.bEnteredTransport = false;
    AI.bVirtualTransport = false;
    AI.bJumpedFromTransport = false;
    AI.bAirPawnSeen = false;
    AI.bThankedDriver = false;
    NavigationBehavior::ClearMoveTarget(AI);

    // Choose where to land before jumping.
    AI.SetState(EPlayerAIState::ChoosingLandingSpot, "transport phase started");
    AI.LandingTarget = LandingBehavior::PickLandingTarget(AI, World);
    AI.bHasLandingTarget = true;

    AIDebugLogger::Log("Transport", "%s chose landing target (%.0f, %.0f)", AI.Entity.DisplayName.c_str(),
        AI.LandingTarget.X, AI.LandingTarget.Y);

    AI.SetState(EPlayerAIState::WaitingForTransport, "landing target chosen");
}

void TransportBehavior::Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    if (!AI.Entity.IsValid())
        return;

    switch (AI.GetState())
    {
    case EPlayerAIState::WaitingForTransport:
    {
        // Parked-seat transport model (works on every version): the warmup
        // pawn stays parked while the AI is "in the bus", and at jump time
        // it is placed at the aircraft's position and skydives down.
        NavigationBehavior::ClearMoveTarget(AI);

        if (AI.Entity.bNativeBacked)
            NativePlayerAIBackend::StopFire(AI);

        AI.bEnteredTransport = true;
        AI.SetState(EPlayerAIState::InTransport, "boarded transport");
        break;
    }

    case EPlayerAIState::InTransport:
    {
        // Thank the driver only when the version supports it.
        if (!AI.bThankedDriver && Now >= AI.ThankDriverTime)
        {
            AI.bThankedDriver = true;

            if (PlayerAIRandChance(PlayerAIConfig::ThankDriverChance))
            {
                if (VersionFeatureAdapter::ThankDriver(AI.Entity.PC))
                    AIDebugLogger::Verbose("Transport", "%s thanked the driver", AI.Entity.DisplayName.c_str());
            }
        }

        bool bWantsJump = Now >= AI.ForcedJumpTime;

        if (!bWantsJump && AI.bHasLandingTarget)
        {
            auto Aircraft = VersionFeatureAdapter::GetAircraft();

            if (Aircraft)
            {
                FVector AircraftLoc = Aircraft->K2_GetActorLocation();
                AircraftLoc.Z = AI.LandingTarget.Z;

                const double Dist = AircraftLoc.GetDistanceTo(AI.LandingTarget);
                const double JumpWindow = PlayerAIConfig::JumpDistanceMin +
                    (PlayerAIConfig::JumpDistanceMax - PlayerAIConfig::JumpDistanceMin) * ((AI.AIIndex * 37) % 100) / 100.0;

                bWantsJump = Dist <= JumpWindow;

                // The drop window is about to close: bail out now. Anyone
                // still "aboard" when the aircraft finishes dies with the
                // spawn island teardown.
                if (!bWantsJump && Aircraft->HasDropEndTime() && Aircraft->DropEndTime > 1.f &&
                    Now >= Aircraft->DropEndTime - 2.f)
                    bWantsJump = true;
            }
        }

        if (bWantsJump)
        {
            AI.JumpedAtTime = Now;
            AI.bJumpedFromTransport = true;

            if (AI.Entity.bNativeBacked)
            {
                // Native bots are not aircraft passengers (they live in
                // AliveBots) - simulated seat + native skydive.
                auto Aircraft = VersionFeatureAdapter::GetAircraft();
                FVector Start = Aircraft ? Aircraft->K2_GetActorLocation() : (AI.LandingTarget + FVector(0, 0, 15000.f));

                if (Start.Z < AI.LandingTarget.Z + 5000.0)
                    Start.Z = AI.LandingTarget.Z + 8000.0;

                NativePlayerAIBackend::SkydiveFrom(AI, Start);
                AI.SetState(EPlayerAIState::Gliding, "jumped from transport");
                break;
            }

            // Simulated tier: PlayerAI are never real aircraft passengers
            // (the EnterAircraft hook skips them - anyone still flagged
            // aboard when the drop zone ends is killed by the native
            // aircraft; the jump RPC also rejects connectionless
            // controllers on old versions). The jump is a fresh pawn at
            // the aircraft that skydives down through the engine's own
            // skydive/glider/landing flow.
            auto OldPawn = AI.GetPawn();

            if (OldPawn)
            {
                // The parked warmup pawn is a leftover ghost.
                OldPawn->K2_DestroyActor();
            }

            if (VersionFeatureAdapter::JumpFromAircraft(AI.Entity.PC) && AI.GetPawn())
            {
                if (MagnesiumPlayerAISpawner::FinishAircraftJumpPawn(AI))
                    AI.SetState(EPlayerAIState::Gliding, "skydiving from aircraft");
                else
                    AI.SetState(EPlayerAIState::SearchingForLoot, "direct landing placement");
            }
            else if (MagnesiumPlayerAISpawner::SpawnPawnAt(AI, AI.LandingTarget, true))
            {
                // No jump pawn on this version: direct landing placement.
                AI.SetState(EPlayerAIState::SearchingForLoot, "direct landing placement");
            }
            else
            {
                AI.bJumpedFromTransport = false; // retry next think
            }
        }
        break;
    }

    case EPlayerAIState::JumpingFromTransport:
    {
        // Waiting for the native jump to hand us the skydiving pawn.
        auto Pawn = AI.GetPawn();

        if (Pawn)
        {
            AI.SetState(EPlayerAIState::Gliding, "skydiving");
            break;
        }

        if (Now - AI.JumpedAtTime > 4.f)
        {
            // The native jump produced nothing: direct placement fallback.
            if (MagnesiumPlayerAISpawner::SpawnPawnAt(AI, AI.LandingTarget, true))
                AI.SetState(EPlayerAIState::SearchingForLoot, "fallback landing placement");
            else
                AI.JumpedAtTime = Now; // retry, never crash
        }
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// LandingBehavior
// ============================================================================

// Force-ends a stuck skydive/glider through the engine's own landing path
// (fault guarded) - without it a pawn whose landing we assisted can stand
// on the ground frozen with the glider still out.
// (Kept free of unwindable C++ objects so SEH is allowed here.)
static void PlayerAITryEndSkydiving(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn || !AFortPlayerPawnAthena::EndSkydivingOG)
        return;

    GPlayerAIGuardedNativeCallDepth++;

    __try
    {
        AFortPlayerPawnAthena::EndSkydivingOG(Pawn);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    GPlayerAIGuardedNativeCallDepth--;
}

static std::vector<FPlayerAILandingCluster> LandingClusters;

std::vector<FPlayerAILandingCluster>& LandingBehavior::GetClusters()
{
    return LandingClusters;
}

void LandingBehavior::Reset()
{
    LandingClusters.clear();
}

void LandingBehavior::BuildClusters(FPlayerAIWorldSnapshot& World)
{
    LandingClusters.clear();

    // Generic map data: cluster loot container locations into loot areas.
    // Works on every version/map - no POI assets required.
    constexpr double CellSize = 16384.0; // ~160m grid cells

    struct FCell { double SumX = 0, SumY = 0, SumZ = 0; int Count = 0; };
    std::unordered_map<long long, FCell> Cells;

    for (auto Container : World.Containers)
    {
        if (!Container)
            continue;

        FVector Loc = Container->K2_GetActorLocation();

        const long long CX = (long long)floor(Loc.X / CellSize);
        const long long CY = (long long)floor(Loc.Y / CellSize);
        const long long Key = CX * 1000003ll + CY;

        auto& Cell = Cells[Key];
        Cell.SumX += Loc.X;
        Cell.SumY += Loc.Y;
        Cell.SumZ += Loc.Z;
        Cell.Count++;
    }

    for (auto& [Key, Cell] : Cells)
    {
        if (Cell.Count < 2)
            continue; // ignore isolated single containers

        FPlayerAILandingCluster Cluster;
        Cluster.Center = FVector(Cell.SumX / Cell.Count, Cell.SumY / Cell.Count, Cell.SumZ / Cell.Count);
        Cluster.LootCount = Cell.Count;
        LandingClusters.push_back(Cluster);
    }

    std::sort(LandingClusters.begin(), LandingClusters.end(),
        [](const FPlayerAILandingCluster& A, const FPlayerAILandingCluster& B) { return A.LootCount > B.LootCount; });

    if (LandingClusters.empty())
        AIDebugLogger::MissingFeature("LandingClusterData", "PlayerAI lands near the map center / player starts");
    else
        AIDebugLogger::Log("Landing", "built %d landing clusters from loot data", (int)LandingClusters.size());
}

FVector LandingBehavior::PickLandingTarget(PlayerAIController& AI, FPlayerAIWorldSnapshot& World)
{
    if (LandingClusters.empty())
        BuildClusters(World);

    FVector Base{};
    bool bHaveBase = false;

    if (!LandingClusters.empty())
    {
        const int Count = (int)LandingClusters.size();
        int Index;

        if (PlayerAIRandChance(AI.Skill.HotDropChance))
        {
            // Popular/dense loot areas.
            const int TopCount = Count < 4 ? Count : (Count / 4 < 1 ? 1 : Count / 4);
            Index = PlayerAIRandInt(0, TopCount - 1);
        }
        else
        {
            // Quieter areas for safer profiles.
            Index = PlayerAIRandInt(Count / 4, Count - 1);

            if (Index >= Count)
                Index = Count - 1;
        }

        Base = LandingClusters[Index].Center;
        bHaveBase = true;

        AIDebugLogger::Verbose("Landing", "%s picked cluster %d (loot %d)", AI.Entity.DisplayName.c_str(),
            Index, LandingClusters[Index].LootCount);
    }

    if (!bHaveBase)
    {
        // Fallbacks: map center, then the AI home location.
        auto GameState = World.GameState;

        if (GameState && GameState->HasMapInfo() && GameState->MapInfo)
        {
            Base = GameState->MapInfo->GetMapCenter();
            bHaveBase = true;
        }
        else
        {
            Base = AI.HomeLocation;
        }
    }

    // Natural spread: jitter inside the chosen area.
    Base.X += PlayerAIRandRange(-PlayerAIConfig::LandingJitterRadius, PlayerAIConfig::LandingJitterRadius);
    Base.Y += PlayerAIRandRange(-PlayerAIConfig::LandingJitterRadius, PlayerAIConfig::LandingJitterRadius);

    bool bFound = false;
    FVector Ground = VersionFeatureAdapter::FindGroundLocation(Base, bFound);

    // Avoid unreachable spots: when no ground exists there, fall back to the
    // cluster/base location itself.
    return bFound ? Ground : Base;
}

bool LandingBehavior::Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    auto Pawn = AI.GetPawn();

    if (Pawn)
        AI.bAirPawnSeen = true;

    if (!Pawn)
    {
        // An airborne pawn existed and is now gone: it died mid-air (fall,
        // void, real damage) through the native pipeline - accept the death
        // so alive counts stay correct. Only when the native jump never
        // produced a pawn do we use the landing placement fallback.
        if (AI.bAirPawnSeen)
        {
            AI.bDeathHandled = true;
            AI.SetState(EPlayerAIState::Dead, "died before landing");
            EliminationBehavior::OnEliminated(AI, Now);
            return false;
        }

        if (Now - AI.JumpedAtTime > 4.f)
        {
            auto NewPawn = MagnesiumPlayerAISpawner::SpawnPawnAt(AI, AI.LandingTarget, true);

            if (NewPawn)
            {
                AI.SetState(EPlayerAIState::SearchingForLoot, "fallback landing");
                return true;
            }
        }
        return false;
    }

    // Mid-air death with the pawn still present goes through the normal
    // death handling (never respawned).
    if (Pawn->GetHealth() <= 0.f)
    {
        DamageBehavior::DetectDeath(AI, Now);
        return false;
    }

    FVector Loc = Pawn->K2_GetActorLocation();

    bool bGroundKnown = false;
    FVector Ground{};
    {
        bool bFound = false;
        Ground = VersionFeatureAdapter::FindGroundLocation(Loc, bFound, Pawn);
        bGroundKnown = bFound;
    }

    // Trace-free anchor: the landing target's Z comes from real container
    // positions, so it IS terrain height at that spot.
    const double AnchorZ = AI.bHasLandingTarget ? (double)AI.LandingTarget.Z : (double)AI.CachedGroundZ;
    const double HeightAboveGround = bGroundKnown ? (Loc.Z - Ground.Z) : (Loc.Z - AnchorZ);

    bool bStillAirborne = false;

    if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
        bStillAirborne = true;
    else
        bStillAirborne = HeightAboveGround > 500.0;

    // Soft landing assist - ONLY for a genuine fast free-fall with no
    // glider. Gliding pawns are engine-managed; interrupting a working
    // glider froze pawns on the ground with the glider still out.
    bool bNeedsAssist = false;

    if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
        bNeedsAssist = Pawn->CharacterMovement->Velocity.Z < -3200.0;

    if (bStillAirborne && bNeedsAssist && HeightAboveGround < 900.0)
    {
        FVector LandSpot = Loc;
        LandSpot.Z = (bGroundKnown ? Ground.Z : AnchorZ) + 100.0;

        Pawn->K2_TeleportTo(LandSpot, Pawn->K2_GetActorRotation(), false, true);
        AI.PosVertVel = 0.f;
        AI.bPosGrounded = false; // swept gravity re-verifies footing

        if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
            Pawn->CharacterMovement->Velocity = FVector{};

        ReplicationBehavior::PushTeleportUpdate(Pawn);
        Loc = LandSpot;
        bStillAirborne = false;
    }

    if (!bStillAirborne)
    {
        // Glider cleanup: if the skydive state is still set once we are on
        // the ground, end it through the engine's own landing path.
        if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
            PlayerAITryEndSkydiving(Pawn);

        AIDebugLogger::Verbose("Landing", "%s landed at (%.0f, %.0f)", AI.Entity.DisplayName.c_str(), Loc.X, Loc.Y);
        AI.CachedGroundZ = bGroundKnown ? (float)Ground.Z : (float)(Loc.Z - PLAYERAI_PAWN_HALF_HEIGHT);
        AI.LootingSinceTime = Now;
        AI.SetState(EPlayerAIState::SearchingForLoot, "landed");
        return true;
    }

    // Air stall detection: if the pawn does not descend for a while,
    // teleport-land as a safe fallback.
    if (Now >= AI.NextStuckCheckTime)
    {
        if (AI.NextStuckCheckTime > 0.f && fabs(Loc.Z - AI.LastStuckCheckLocation.Z) < 50.0)
        {
            AIDebugLogger::Log("Landing", "%s air-stalled - using landing placement fallback", AI.Entity.DisplayName.c_str());

            if (MagnesiumPlayerAISpawner::SpawnPawnAt(AI, AI.LandingTarget, true))
            {
                AI.SetState(EPlayerAIState::SearchingForLoot, "air stall fallback");
                return true;
            }
        }

        AI.LastStuckCheckLocation = Loc;
        AI.NextStuckCheckTime = Now + 3.f;
    }

    return false;
}

// ============================================================================
// InventoryBehavior
// ============================================================================

// Inventory source differs per backend: simulated AI use the player world
// inventory, native bots use the bot controller's own inventory.
static AFortInventory* PlayerAIGetInventory(PlayerAIController& AI)
{
    if (AI.Entity.bNativeBacked)
    {
        auto Bot = NativePlayerAIBackend::GetBotController(AI);
        return Bot && Bot->HasInventory() ? Bot->Inventory : nullptr;
    }

    return AI.Entity.PC ? AI.Entity.PC->WorldInventory : nullptr;
}

// Loaded ammo of the currently equipped entry (-1 when unknown) - used to
// detect whether real trigger fire consumes ammo on this version.
static int PlayerAIGetEquippedLoadedAmmo(PlayerAIController& AI)
{
    auto Inventory = PlayerAIGetInventory(AI);

    if (!Inventory)
        return -1;

    auto& Entries = Inventory->Inventory.ReplicatedEntries;

    for (int i = 0; i < Entries.Num(); i++)
    {
        auto Entry = (FFortItemEntry*)((PBYTE)Entries.GetData() + (i * FFortItemEntry::Size()));

        if (!Entry)
            continue;

        if (Entry->ItemGuid == AI.EquippedItemGuid)
            return Entry->HasLoadedAmmo() ? Entry->LoadedAmmo : -1;
    }

    return -1;
}

static void PlayerAIEquipEntry(PlayerAIController& AI, FFortItemEntry* Entry)
{
    if (!Entry || !Entry->ItemDefinition)
        return;

    if (AI.Entity.bNativeBacked)
    {
        auto Pawn = AI.GetPawn();

        if (Pawn)
        {
            auto Weapon = (AFortWeapon*)Pawn->EquipWeaponDefinition(
                (UFortItemDefinition*)Entry->ItemDefinition,
                Entry->ItemGuid);
            if (Weapon)
            {
                FFortWeaponMods::ApplyEntrySlotsAfterEquip(
                    Weapon, *Entry);
            }
        }
        return;
    }

    auto PC = AI.Entity.PC;

    if (!PC)
        return;

    PC->ServerExecuteInventoryItem(Entry->ItemGuid);
    PC->ClientEquipItem(Entry->ItemGuid, true);
}

static std::string PlayerAIToLowerName(const UFortItemDefinition* ItemDef)
{
    if (!ItemDef)
        return "";

    auto RawName = ItemDef->Name.ToString();
    std::string Name(RawName.c_str());
    std::transform(Name.begin(), Name.end(), Name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return Name;
}

EPlayerAIWeaponRole InventoryBehavior::ClassifyItem(const UFortItemDefinition* ItemDef)
{
    if (!ItemDef)
        return EPlayerAIWeaponRole::None;

    static auto MeleeClass = FindClass("FortWeaponMeleeItemDefinition");
    static auto RangedClass = FindClass("FortWeaponRangedItemDefinition");
    static auto AmmoClass = FindClass("FortAmmoItemDefinition");
    static auto ResourceClass = FindClass("FortResourceItemDefinition");

    if (MeleeClass && ItemDef->IsA(MeleeClass))
        return EPlayerAIWeaponRole::None; // pickaxe

    if (ResourceClass && ItemDef->IsA(ResourceClass))
        return EPlayerAIWeaponRole::Resource;

    if (AmmoClass && ItemDef->IsA(AmmoClass))
        return EPlayerAIWeaponRole::Ammo;

    const std::string Name = PlayerAIToLowerName(ItemDef);

    auto Contains = [&](const char* Sub) { return Name.find(Sub) != std::string::npos; };

    if (Contains("ammo"))
        return EPlayerAIWeaponRole::Ammo;

    if (Contains("shield") || Contains("slurp") || Contains("chugjug"))
        return EPlayerAIWeaponRole::Shield;

    if (Contains("bandage") || Contains("medkit") || Contains("medic") || Contains("cozy") || Contains("campfire"))
        return EPlayerAIWeaponRole::Healing;

    const bool bIsRanged = RangedClass && ItemDef->IsA(RangedClass);

    if (Contains("shotgun") || Contains("smg") || Contains("submachine"))
        return EPlayerAIWeaponRole::CloseRange;

    if (Contains("sniper") || Contains("boltaction") || Contains("huntingrifle") || Contains("dmr"))
        return EPlayerAIWeaponRole::LongRange;

    if (Contains("launcher") || Contains("rocket") || Contains("grenade") || Contains("c4") || Contains("clinger") || Contains("dynamite"))
        return EPlayerAIWeaponRole::Explosive;

    if (Contains("assault") || Contains("rifle") || Contains("burst") || Contains("pistol") ||
        Contains("revolver") || Contains("handcannon") || Contains("sixshooter") || Contains("scoped"))
        return EPlayerAIWeaponRole::MediumRange;

    if (bIsRanged)
        return EPlayerAIWeaponRole::MediumRange;

    // Unknown / version specific items are safely treated as utility;
    // unsupported ones simply never crash the AI.
    return EPlayerAIWeaponRole::Unknown;
}

static int PlayerAIGetRarity(const UFortItemDefinition* ItemDef)
{
    if (!ItemDef || !ItemDef->HasRarity())
        return 0;

    return (int)ItemDef->Rarity;
}

bool InventoryBehavior::WantsItem(PlayerAIController& AI, const UFortItemDefinition* ItemDef)
{
    const auto Role = ClassifyItem(ItemDef);

    switch (Role)
    {
    case EPlayerAIWeaponRole::CloseRange:
        return true;
    case EPlayerAIWeaponRole::MediumRange:
        return true;
    case EPlayerAIWeaponRole::LongRange:
    case EPlayerAIWeaponRole::Explosive:
        return true;
    case EPlayerAIWeaponRole::Healing:
        return AI.HealingItemCount < 3;
    case EPlayerAIWeaponRole::Shield:
        return AI.ShieldItemCount < 3;
    case EPlayerAIWeaponRole::Ammo:
        return true;
    default:
        return false; // duplicates of unknown/utility items are skipped
    }
}

bool InventoryBehavior::TakePickup(PlayerAIController& AI, AFortPickupAthena* Pickup)
{
    if (!Pickup)
        return false;

    if (Pickup->HasbPickedUp() && Pickup->bPickedUp)
        return false;

    auto Inventory = PlayerAIGetInventory(AI);

    if (!Inventory)
        return false;

    auto& Entry = Pickup->PrimaryPickupItemEntry;

    if (!Entry.ItemDefinition)
        return false;

    if (!WantsItem(AI, Entry.ItemDefinition))
        return false;

    Inventory->GiveItem(Entry);

    if (Pickup->HasbPickedUp())
    {
        Pickup->bPickedUp = true;
        Pickup->OnRep_bPickedUp();
    }

    Pickup->SetLifeSpan(1.0f);

    AIDebugLogger::Verbose("Inventory", "%s picked up %s", AI.Entity.DisplayName.c_str(), Entry.ItemDefinition->Name.ToString().c_str());
    return true;
}

void InventoryBehavior::RefreshAndEquipBest(PlayerAIController& AI, bool bPreferCloseRange)
{
    auto Inventory = PlayerAIGetInventory(AI);

    if (!Inventory)
        return;

    auto& Entries = Inventory->Inventory.ReplicatedEntries;

    AI.bHasCloseRange = false;
    AI.bHasMediumRange = false;
    AI.HealingItemCount = 0;
    AI.ShieldItemCount = 0;

    FFortItemEntry* BestClose = nullptr;
    FFortItemEntry* BestMedium = nullptr;
    FFortItemEntry* BestAny = nullptr;
    FFortItemEntry* Pickaxe = nullptr;
    int BestCloseRarity = -1, BestMediumRarity = -1, BestAnyRarity = -1;

    static auto MeleeClass = FindClass("FortWeaponMeleeItemDefinition");

    for (int i = 0; i < Entries.Num(); i++)
    {
        auto Entry = (FFortItemEntry*)((PBYTE)Entries.GetData() + (i * FFortItemEntry::Size()));

        if (!Entry || !Entry->ItemDefinition)
            continue;

        if (MeleeClass && Entry->ItemDefinition->IsA(MeleeClass))
        {
            if (!Pickaxe)
                Pickaxe = Entry;
            continue;
        }

        const auto Role = ClassifyItem(Entry->ItemDefinition);
        const int Rarity = PlayerAIGetRarity(Entry->ItemDefinition);

        switch (Role)
        {
        case EPlayerAIWeaponRole::CloseRange:
            AI.bHasCloseRange = true;
            if (Rarity > BestCloseRarity) { BestCloseRarity = Rarity; BestClose = Entry; }
            if (Rarity > BestAnyRarity) { BestAnyRarity = Rarity; BestAny = Entry; }
            break;
        case EPlayerAIWeaponRole::MediumRange:
        case EPlayerAIWeaponRole::LongRange:
        case EPlayerAIWeaponRole::Explosive:
            if (Role == EPlayerAIWeaponRole::MediumRange)
            {
                AI.bHasMediumRange = true;
                if (Rarity > BestMediumRarity) { BestMediumRarity = Rarity; BestMedium = Entry; }
            }
            if (Rarity > BestAnyRarity) { BestAnyRarity = Rarity; BestAny = Entry; }
            break;
        case EPlayerAIWeaponRole::Healing:
            AI.HealingItemCount += Entry->HasCount() ? Entry->Count : 1;
            break;
        case EPlayerAIWeaponRole::Shield:
            AI.ShieldItemCount += Entry->HasCount() ? Entry->Count : 1;
            break;
        default:
            break;
        }
    }

    FFortItemEntry* ToEquip = nullptr;

    if (bPreferCloseRange && BestClose)
        ToEquip = BestClose;
    else if (BestMedium)
        ToEquip = BestMedium;
    else if (BestAny)
        ToEquip = BestAny;
    else
        ToEquip = Pickaxe;

    if (!ToEquip || !ToEquip->ItemDefinition)
        return;

    if (AI.EquippedItemGuid == ToEquip->ItemGuid)
        return; // already holding the best option

    // Weapon swap skill: lower skilled AI sometimes keep a worse weapon.
    if (AI.EquippedRole != EPlayerAIWeaponRole::None && !PlayerAIRandChance(AI.Skill.WeaponSwapSkill))
        return;

    PlayerAIEquipEntry(AI, ToEquip);

    AI.EquippedItemGuid = ToEquip->ItemGuid;
    AI.EquippedRole = ClassifyItem(ToEquip->ItemDefinition);
    AI.MagazineRemaining = PlayerAIConfig::MagazineSizeDefault;

    AIDebugLogger::Verbose("Inventory", "%s equipped %s", AI.Entity.DisplayName.c_str(), ToEquip->ItemDefinition->Name.ToString().c_str());
}

bool InventoryBehavior::EquipHealingItem(PlayerAIController& AI, bool bShield)
{
    auto Inventory = PlayerAIGetInventory(AI);

    if (!Inventory)
        return false;

    auto& Entries = Inventory->Inventory.ReplicatedEntries;

    // Preferred type first, the other kind as fallback.
    for (int Pass = 0; Pass < 2; Pass++)
    {
        const auto WantedRole = (Pass == 0) == bShield ? EPlayerAIWeaponRole::Shield : EPlayerAIWeaponRole::Healing;

        for (int i = 0; i < Entries.Num(); i++)
        {
            auto Entry = (FFortItemEntry*)((PBYTE)Entries.GetData() + (i * FFortItemEntry::Size()));

            if (!Entry || !Entry->ItemDefinition)
                continue;

            if (ClassifyItem(Entry->ItemDefinition) != WantedRole)
                continue;

            PlayerAIEquipEntry(AI, Entry);

            AI.EquippedItemGuid = Entry->ItemGuid;
            AI.EquippedRole = WantedRole;
            return true;
        }
    }

    return false;
}

float InventoryBehavior::ConsumeHealingItem(PlayerAIController& AI, bool bShield)
{
    auto Inventory = PlayerAIGetInventory(AI);

    if (!Inventory)
        return 0.f;

    auto& Entries = Inventory->Inventory.ReplicatedEntries;

    for (int i = 0; i < Entries.Num(); i++)
    {
        auto Entry = (FFortItemEntry*)((PBYTE)Entries.GetData() + (i * FFortItemEntry::Size()));

        if (!Entry || !Entry->ItemDefinition)
            continue;

        const auto Role = ClassifyItem(Entry->ItemDefinition);

        if ((bShield && Role != EPlayerAIWeaponRole::Shield) || (!bShield && Role != EPlayerAIWeaponRole::Healing))
            continue;

        if (Entry->HasCount() && Entry->Count > 1)
        {
            Entry->Count = Entry->Count - 1;
            Inventory->UpdateEntry(*Entry);
        }
        else
        {
            Inventory->Remove(Entry->ItemGuid);
        }

        if (bShield)
            AI.ShieldItemCount = AI.ShieldItemCount > 0 ? AI.ShieldItemCount - 1 : 0;
        else
            AI.HealingItemCount = AI.HealingItemCount > 0 ? AI.HealingItemCount - 1 : 0;

        return bShield ? PlayerAIConfig::ShieldPotionAmount : PlayerAIConfig::HealAmount;
    }

    return 0.f;
}

// ============================================================================
// LootingBehavior
// ============================================================================

bool LootingBehavior::AcquireLootTarget(PlayerAIController& AI, FPlayerAIWorldSnapshot& World)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return false;

    FVector Loc = Pawn->K2_GetActorLocation();

    AI.LootContainerTarget = nullptr;
    AI.LootPickupTarget = nullptr;

    double BestScore = PlayerAIConfig::LootSearchRadius;

    for (auto Pickup : World.Pickups)
    {
        if (!Pickup)
            continue;

        if (Pickup->HasbPickedUp() && Pickup->bPickedUp)
            continue;

        auto& Entry = Pickup->PrimaryPickupItemEntry;

        if (!Entry.ItemDefinition || !InventoryBehavior::WantsItem(AI, Entry.ItemDefinition))
            continue;

        FVector PickupLoc = Pickup->K2_GetActorLocation();
        const double Dist = Loc.GetDistanceTo(PickupLoc) * 0.8; // slight preference for open loot

        if (Dist < BestScore)
        {
            BestScore = Dist;
            AI.LootPickupTarget = Pickup;
            AI.LootContainerTarget = nullptr;
            AI.LootTargetLocation = PickupLoc;
        }
    }

    for (auto Container : World.Containers)
    {
        if (!Container)
            continue;

        if (Container->HasbAlreadySearched() && Container->bAlreadySearched)
            continue;

        FVector ContainerLoc = Container->K2_GetActorLocation();
        const double Dist = Loc.GetDistanceTo(ContainerLoc);

        if (Dist < BestScore)
        {
            BestScore = Dist;
            AI.LootContainerTarget = Container;
            AI.LootPickupTarget = nullptr;
            AI.LootTargetLocation = ContainerLoc;
        }
    }

    if (AI.LootContainerTarget || AI.LootPickupTarget)
    {
        AIDebugLogger::Verbose("Loot", "%s loot target at (%.0f, %.0f) [%s]", AI.Entity.DisplayName.c_str(),
            AI.LootTargetLocation.X, AI.LootTargetLocation.Y, AI.LootContainerTarget ? "container" : "pickup");
        return true;
    }

    return false;
}

static void PlayerAIGrabNearbyPickups(PlayerAIController& AI, float Radius)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    FVector Loc = Pawn->K2_GetActorLocation();

    TArray<AFortPickupAthena*> AllPickups;
    Utils::GetAll<AFortPickupAthena>(AllPickups);

    int Taken = 0;

    for (auto& Pickup : AllPickups)
    {
        if (!Pickup || Taken >= 4)
            break;

        if (Pickup->HasbPickedUp() && Pickup->bPickedUp)
            continue;

        if (Loc.GetDistanceTo(Pickup->K2_GetActorLocation()) > Radius)
            continue;

        if (InventoryBehavior::TakePickup(AI, Pickup))
            Taken++;
    }

    AllPickups.Free();
}

void LootingBehavior::Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    switch (AI.GetState())
    {
    case EPlayerAIState::SearchingForLoot:
    {
        // Enough loot for this AI's greed? Start playing the match.
        const bool bArmed = AI.bHasCloseRange || AI.bHasMediumRange;
        const float LootTime = 20.f + AI.Skill.LootGreed * 60.f;

        if (bArmed && (Now - AI.LootingSinceTime) > LootTime)
        {
            AI.SetState(EPlayerAIState::SearchingForEnemies, "done looting");
            break;
        }

        if (AcquireLootTarget(AI, World))
        {
            NavigationBehavior::SetMoveTarget(AI, AI.LootTargetLocation);
            AI.SetState(EPlayerAIState::MovingToLoot, "loot found");
            break;
        }

        // No loot nearby: drift toward the safe zone (or wander).
        if (World.bHasSafeZone)
        {
            FVector Toward = Pawn->K2_GetActorLocation();
            FVector Dir = World.SafeZoneCenter - Toward;
            Dir.Z = 0;

            const double Dist = Dir.Magnitude();

            if (Dist > 2000.0)
            {
                Dir = Dir / Dist;
                NavigationBehavior::SetMoveTarget(AI, NavigationBehavior::RandomPointNear(Toward + Dir * 4000.0, 1500.f));
                break;
            }
        }

        NavigationBehavior::SetMoveTarget(AI, NavigationBehavior::RandomPointNear(Pawn->K2_GetActorLocation(), 5000.f));
        break;
    }

    case EPlayerAIState::MovingToLoot:
    {
        // Re-validate the target (someone else may have taken it).
        const bool bContainerValid = AI.LootContainerTarget && !(AI.LootContainerTarget->HasbAlreadySearched() && AI.LootContainerTarget->bAlreadySearched);
        const bool bPickupValid = AI.LootPickupTarget && !(AI.LootPickupTarget->HasbPickedUp() && AI.LootPickupTarget->bPickedUp);

        if (!bContainerValid && !bPickupValid)
        {
            AI.LootContainerTarget = nullptr;
            AI.LootPickupTarget = nullptr;
            AI.SetState(EPlayerAIState::SearchingForLoot, "loot target gone");
            break;
        }

        FVector Loc = Pawn->K2_GetActorLocation();
        const double Dist = Loc.GetDistanceTo(AI.LootTargetLocation);
        const double InteractRange = bContainerValid ? PlayerAIConfig::ContainerInteractRange : PlayerAIConfig::PickupInteractRange;

        if (Dist <= InteractRange)
        {
            NavigationBehavior::ClearMoveTarget(AI);

            if (bContainerValid)
            {
                AI.ActionEndTime = Now + PlayerAIConfig::OpenContainerDuration;
                AI.SetState(EPlayerAIState::OpeningContainer, "at container");
            }
            else
            {
                AI.SetState(EPlayerAIState::PickingUpItem, "at pickup");
            }
            break;
        }

        if (!AI.bHasMoveTarget)
            NavigationBehavior::SetMoveTarget(AI, AI.LootTargetLocation);

        NavigationBehavior::CheckStuck(AI, Now);
        break;
    }

    case EPlayerAIState::OpeningContainer:
    {
        if (Now < AI.ActionEndTime)
            break;

        auto Container = AI.LootContainerTarget;
        AI.LootContainerTarget = nullptr;

        if (Container && !(Container->HasbAlreadySearched() && Container->bAlreadySearched))
        {
            // Same server side search path the interact handler uses -
            // generic across chests / ammo boxes / version equivalents.
            UFortLootPackage::SpawnLootHook(Container);
            AIDebugLogger::Verbose("Loot", "%s opened a container", AI.Entity.DisplayName.c_str());
        }

        AI.SetState(EPlayerAIState::PickingUpItem, "container opened");
        break;
    }

    case EPlayerAIState::PickingUpItem:
    {
        if (AI.LootPickupTarget)
        {
            InventoryBehavior::TakePickup(AI, AI.LootPickupTarget);
            AI.LootPickupTarget = nullptr;
        }

        // Grab whatever just tossed out of a container / lies at our feet.
        PlayerAIGrabNearbyPickups(AI, 500.f);

        AI.SetState(EPlayerAIState::ManagingInventory, "picked up items");
        break;
    }

    case EPlayerAIState::ManagingInventory:
    {
        InventoryBehavior::RefreshAndEquipBest(AI, false);
        AI.SetState(EPlayerAIState::SearchingForLoot, "inventory managed");
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// ZoneRotationBehavior
// ============================================================================

bool ZoneRotationBehavior::ShouldRotate(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    if (!World.bHasSafeZone)
        return false;

    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return false;

    FVector Loc = Pawn->K2_GetActorLocation();
    FVector Flat = Loc;
    Flat.Z = World.SafeZoneCenter.Z;

    const double Dist = Flat.GetDistanceTo(World.SafeZoneCenter);

    // Storm awareness: skilled AI rotate earlier (at 60% of the radius),
    // careless ones wait until they are nearly in the storm.
    const double Threshold = World.SafeZoneRadius * (0.60 + 0.38 * (1.0 - AI.Skill.StormAwareness));

    return Dist > Threshold;
}

void ZoneRotationBehavior::Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    if (!World.bHasSafeZone)
    {
        AI.SetState(EPlayerAIState::SearchingForLoot, "no zone data");
        return;
    }

    FVector Loc = Pawn->K2_GetActorLocation();
    FVector Flat = Loc;
    Flat.Z = World.SafeZoneCenter.Z;
    const double Dist = Flat.GetDistanceTo(World.SafeZoneCenter);

    if (Dist <= World.SafeZoneRadius * 0.55)
    {
        // Safely inside: heal up if hurt, otherwise resume playing.
        NavigationBehavior::ClearMoveTarget(AI);

        const float Health = Pawn->GetHealth();
        const float Shield = Pawn->GetShield();

        if (((Health < 75.f && AI.HealingItemCount > 0) || (Shield < 50.f && AI.ShieldItemCount > 0)) &&
            PlayerAIRandChance(AI.Skill.HealingDiscipline))
        {
            AI.ActionEndTime = Now + PlayerAIConfig::HealDuration;
            AI.SetState(EPlayerAIState::Healing, "healing after rotation");
        }
        else
        {
            AI.LootingSinceTime = Now;
            AI.SetState(PlayerAIRandChance(AI.Skill.PushChance) ? EPlayerAIState::SearchingForEnemies : EPlayerAIState::SearchingForLoot, "inside zone");
        }
        return;
    }

    if (!AI.bHasMoveTarget)
    {
        // Rotate to a random point well inside the target circle.
        FVector Target = World.SafeZoneCenter;
        const float Angle = PlayerAIRandRange(0.f, 6.2831853f);
        const double R = World.SafeZoneRadius * PlayerAIRandRange(0.15f, 0.45f);
        Target.X += cos(Angle) * R;
        Target.Y += sin(Angle) * R;

        bool bFound = false;
        FVector Ground = VersionFeatureAdapter::FindGroundLocation(Target, bFound, Pawn);
        NavigationBehavior::SetMoveTarget(AI, bFound ? Ground : Target);

        AIDebugLogger::Verbose("Zone", "%s rotating to zone (%.0f, %.0f)", AI.Entity.DisplayName.c_str(), Target.X, Target.Y);
    }

    NavigationBehavior::CheckStuck(AI, Now);
}

void ZoneRotationBehavior::TickStormDamage(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn || !World.bHasSafeZone)
        return;

    const float Health = Pawn->GetHealth();

    if (Health <= 0.f)
        return;

    // The fallback may only ever damage once the storm has actually CLOSED
    // on its target circle. Before that, standing outside the next circle
    // is completely safe - damaging there was killing PlayerAI shortly
    // after they landed.
    bool bStormClosed = false;

    if (World.GameMode && World.GameMode->HasSafeZoneIndicator() && World.GameMode->SafeZoneIndicator)
    {
        auto Indicator = World.GameMode->SafeZoneIndicator;

        if (Indicator->HasSafeZoneFinishShrinkTime())
        {
            const float Finish = Indicator->SafeZoneFinishShrinkTime;
            bStormClosed = Finish > 1.f && Now >= Finish;
        }
    }

    if (!bStormClosed)
    {
        AI.bOutsideZone = false;
        AI.bStormFallbackActive = false;
        return;
    }

    const bool bInside = VersionFeatureAdapter::IsInsideSafeZone(Pawn->K2_GetActorLocation());

    if (bInside)
    {
        AI.bOutsideZone = false;
        AI.bStormFallbackActive = false;
        return;
    }

    if (!AI.bOutsideZone)
    {
        AI.bOutsideZone = true;
        AI.OutsideZoneSince = Now;
        AI.LastObservedHealth = Health + Pawn->GetShield();
        AI.bStormFallbackActive = false;
        return;
    }

    if (!AI.bStormFallbackActive)
    {
        const float Observed = Health + Pawn->GetShield();

        if (Observed < AI.LastObservedHealth - 0.5f)
        {
            // Native storm damage is affecting this AI - nothing to do.
            AI.LastObservedHealth = Observed;
            AI.OutsideZoneSince = Now;
            return;
        }

        if (Now - AI.OutsideZoneSince < PlayerAIConfig::StormFallbackActivationDelay)
            return;

        AI.bStormFallbackActive = true;
        AI.LastStormDamageTime = Now;
        AIDebugLogger::MissingFeature("NativeStormDamageForPlayerAI", "applying PlayerAI fallback storm damage");
    }

    if (Now - AI.LastStormDamageTime >= PlayerAIConfig::StormFallbackDamageInterval)
    {
        AI.LastStormDamageTime = Now;
        DamageBehavior::ApplyEnvironmentalDamage(AI, VersionFeatureAdapter::GetStormDamagePerSecond());
    }
}

// ============================================================================
// CombatBehavior
// ============================================================================

bool CombatBehavior::IsValidEnemy(PlayerAIController& AI, AFortPlayerControllerAthena* Enemy)
{
    if (!Enemy || Enemy == AI.Entity.PC)
        return false;

    // NOTE: only AController base properties (Pawn/PlayerState) are safe
    // here - the enemy may be a real player controller, a simulated PlayerAI
    // controller or a native bot controller.
    auto EnemyPawn = Enemy->Pawn;

    if (!EnemyPawn || EnemyPawn->GetHealth() <= 0.f)
        return false;

    // Never target players who are still skydiving/gliding (bus jumpers).
    if (EnemyPawn->HasbIsSkydiving() && EnemyPawn->bIsSkydiving)
        return false;

    auto MyPS = AI.Entity.PlayerState;
    auto EnemyPS = (AFortPlayerStateAthena*)Enemy->PlayerState;

    if (!EnemyPS)
        return false;

    if (EnemyPS->HasbIsSpectator() && EnemyPS->bIsSpectator)
        return false;

    if (MyPS && MyPS->TeamIndex == EnemyPS->TeamIndex)
        return false;

    return true;
}

AFortPlayerControllerAthena* CombatBehavior::DetectEnemy(PlayerAIController& AI, FPlayerAIWorldSnapshot& World)
{
    // No combat while mid-drop: skydiving/gliding AI never pick fights.
    if (PlayerAIIsAirState(AI.GetState()))
        return nullptr;

    auto Pawn = AI.GetPawn();

    if (!Pawn || !World.GameMode)
        return nullptr;

    if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
        return nullptr;

    FVector Loc = Pawn->K2_GetActorLocation();

    AFortPlayerControllerAthena* Best = nullptr;
    double BestDist = AI.Skill.DetectionRange;

    // ONE shared detection path for real human players, simulated PlayerAI
    // and native-bot PlayerAI: both alive lists are scanned.
    auto ScanList = [&](TArray<AActor*>& List)
        {
            for (auto& Uncasted : List)
            {
                auto Enemy = (AFortPlayerControllerAthena*)Uncasted;

                if (!IsValidEnemy(AI, Enemy))
                    continue;

                auto EnemyPawn = Enemy->Pawn;
                const double Dist = Loc.GetDistanceTo(EnemyPawn->K2_GetActorLocation());

                if (Dist >= BestDist)
                    continue;

                // Distance based spotting: far targets are noticed less
                // reliably (stands in for visibility/sound cues).
                const float SpotChance = Dist < 4000.0 ? 1.f : (float)(1.0 - (Dist / AI.Skill.DetectionRange) * 0.6);

                if (!PlayerAIRandChance(SpotChance))
                    continue;

                BestDist = Dist;
                Best = Enemy;
            }
        };

    ScanList(World.GameMode->AlivePlayers);

    if (World.GameMode->HasAliveBots())
        ScanList(World.GameMode->AliveBots);

    return Best;
}

static double PlayerAIPreferredCombatRange(EPlayerAIWeaponRole Role)
{
    switch (Role)
    {
    case EPlayerAIWeaponRole::CloseRange: return 700.0;
    case EPlayerAIWeaponRole::LongRange: return 9000.0;
    case EPlayerAIWeaponRole::Explosive: return 3500.0;
    default: return 2500.0;
    }
}

// Maximum distance at which simulated shots may apply damage - shots are
// not real tracer fire (see the TODO in CombatBehavior::Tick), so damage is
// limited to distances where being hit feels plausible.
static double PlayerAIMaxDamageRange(EPlayerAIWeaponRole Role)
{
    switch (Role)
    {
    case EPlayerAIWeaponRole::CloseRange: return 1400.0;   // ~14m
    case EPlayerAIWeaponRole::LongRange: return 14000.0;   // ~140m
    case EPlayerAIWeaponRole::MediumRange: return 8000.0;  // ~80m
    case EPlayerAIWeaponRole::Explosive: return 6000.0;
    default: return 4000.0;
    }
}

static float PlayerAIFireInterval(EPlayerAIWeaponRole Role)
{
    switch (Role)
    {
    case EPlayerAIWeaponRole::CloseRange: return PlayerAIConfig::FireIntervalCloseRange;
    case EPlayerAIWeaponRole::LongRange: return PlayerAIConfig::FireIntervalLongRange;
    default: return PlayerAIConfig::FireIntervalMediumRange;
    }
}

static float PlayerAIWeaponDamage(EPlayerAIWeaponRole Role)
{
    switch (Role)
    {
    case EPlayerAIWeaponRole::CloseRange: return PlayerAIConfig::DamageCloseRange;
    case EPlayerAIWeaponRole::LongRange: return PlayerAIConfig::DamageLongRange;
    case EPlayerAIWeaponRole::MediumRange: return PlayerAIConfig::DamageMediumRange;
    default: return PlayerAIConfig::DamageFallback;
    }
}

void CombatBehavior::Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    switch (AI.GetState())
    {
    case EPlayerAIState::SearchingForEnemies:
    {
        AI.CombatState = EPlayerAICombatState::Searching;

        auto Enemy = DetectEnemy(AI, World);

        if (Enemy)
        {
            AI.CombatTarget = Enemy;
            AI.TargetLastSeenTime = Now;
            AI.ReactionReadyTime = Now + AI.Skill.ReactionTimeSeconds * PlayerAIRandRange(0.75f, 1.35f);
            AI.CombatState = EPlayerAICombatState::SpottingEnemy;
            InventoryBehavior::RefreshAndEquipBest(AI, false);
            AI.SetState(EPlayerAIState::EngagingEnemy, "enemy spotted");

            AIDebugLogger::Verbose("Combat", "%s spotted an enemy", AI.Entity.DisplayName.c_str());
            break;
        }

        // Third party: move toward a nearby fight another PlayerAI is in.
        if (PlayerAIRandChance(AI.Skill.ThirdPartyChance * 0.2f))
        {
            for (auto& Other : PlayerAIManager::GetControllers())
            {
                if (Other.get() == &AI || !Other->IsAlive())
                    continue;

                if (Other->GetState() != EPlayerAIState::EngagingEnemy)
                    continue;

                auto OtherPawn = Other->GetPawn();

                if (!OtherPawn)
                    continue;

                FVector FightLoc = OtherPawn->K2_GetActorLocation();

                if (Pawn->K2_GetActorLocation().GetDistanceTo(FightLoc) < AI.Skill.DetectionRange * 1.5)
                {
                    NavigationBehavior::SetMoveTarget(AI, NavigationBehavior::RandomPointNear(FightLoc, 1500.f));
                    AI.CombatState = EPlayerAICombatState::ThirdPartying;
                    AI.SetState(EPlayerAIState::ThirdPartying, "third partying a fight");
                    break;
                }
            }

            if (AI.GetState() == EPlayerAIState::ThirdPartying)
                break;
        }

        // No enemies: keep moving with the match (loot or rotate).
        if (!AI.bHasMoveTarget)
            AI.SetState(EPlayerAIState::SearchingForLoot, "no enemies around");
        else
            NavigationBehavior::CheckStuck(AI, Now);
        break;
    }

    case EPlayerAIState::EngagingEnemy:
    {
        if (!IsValidEnemy(AI, AI.CombatTarget))
        {
            if (AI.CombatTarget)
                EliminationBehavior::OnEliminatedOther(AI, AI.CombatTarget);

            AI.CombatTarget = nullptr;
            AI.CombatState = EPlayerAICombatState::Disengaging;
            AI.SetState(EPlayerAIState::ManagingInventory, "target gone");
            break;
        }

        if (Now - AI.TargetLastSeenTime > PlayerAIConfig::CombatGiveUpTime)
        {
            AI.CombatTarget = nullptr;
            AI.CombatState = EPlayerAICombatState::Disengaging;
            AI.SetState(EPlayerAIState::SearchingForLoot, "lost the target");
            break;
        }

        const float Health = Pawn->GetHealth() + Pawn->GetShield();

        // Bad fight? Skill decides between retreating and committing.
        if (Health < PlayerAIConfig::HealthLowThreshold && PlayerAIRandChance(AI.Skill.RetreatChance))
        {
            AI.CombatState = EPlayerAICombatState::Retreating;
            AI.SetState(EPlayerAIState::Retreating, "low health");
            break;
        }

        auto EnemyPawn = AI.CombatTarget->Pawn;

        if (!EnemyPawn)
            break;

        AI.LastKnownTargetLocation = EnemyPawn->K2_GetActorLocation();
        AI.TargetLastSeenTime = Now;

        FVector Loc = Pawn->K2_GetActorLocation();
        const double Dist = Loc.GetDistanceTo(AI.LastKnownTargetLocation);
        const bool bWantsClose = Dist < 1800.0 && AI.bHasCloseRange;

        InventoryBehavior::RefreshAndEquipBest(AI, bWantsClose);

        const double Preferred = PlayerAIPreferredCombatRange(AI.EquippedRole);

        if (Dist > AI.Skill.EngageRange || (Dist > Preferred * 2.0 && PlayerAIRandChance(AI.Skill.PushChance)))
        {
            // Push toward the enemy.
            AI.CombatState = EPlayerAICombatState::Pushing;
            NavigationBehavior::SetMoveTarget(AI, NavigationBehavior::RandomPointNear(AI.LastKnownTargetLocation, 900.f));
        }
        else if (Dist < Preferred * 0.5)
        {
            // Back off / strafe to keep spacing.
            FVector Away = Loc - AI.LastKnownTargetLocation;
            Away.Z = 0;
            const double AwayLen = Away.Magnitude();

            if (AwayLen > 1.0)
            {
                Away = Away / AwayLen;
                NavigationBehavior::SetMoveTarget(AI, Loc + Away * 800.0);
            }

            AI.CombatState = EPlayerAICombatState::Engaging;
        }
        else
        {
            // Hold with light strafing; occasionally take cover.
            if (PlayerAIRandChance(0.15f))
            {
                AI.CombatState = EPlayerAICombatState::TakingCover;
                AI.SetState(EPlayerAIState::TakingCover, "briefly taking cover");
                AI.ActionEndTime = Now + PlayerAIRandRange(0.6f, 1.6f);
                NavigationBehavior::SetMoveTarget(AI, NavigationBehavior::RandomPointNear(Loc, 600.f));
                break;
            }

            AI.CombatState = EPlayerAICombatState::Engaging;

            if (!AI.bHasMoveTarget && PlayerAIRandChance(0.6f))
                NavigationBehavior::SetMoveTarget(AI, NavigationBehavior::RandomPointNear(Loc, 500.f));
        }

        NavigationBehavior::CheckStuck(AI, Now);
        break;
    }

    case EPlayerAIState::TakingCover:
    {
        if (Now >= AI.ActionEndTime)
            AI.SetState(EPlayerAIState::EngagingEnemy, "cover pause over");
        break;
    }

    case EPlayerAIState::Reloading:
    {
        AI.CombatState = EPlayerAICombatState::Reloading;

        if (Now >= AI.ActionEndTime)
        {
            AI.MagazineRemaining = PlayerAIConfig::MagazineSizeDefault;
            AI.SetState(AI.CombatTarget ? EPlayerAIState::EngagingEnemy : EPlayerAIState::SearchingForEnemies, "reloaded");
        }
        break;
    }

    case EPlayerAIState::Healing:
    {
        AI.CombatState = EPlayerAICombatState::Healing;
        NavigationBehavior::ClearMoveTarget(AI);

        // Hold the healing item in hand while healing so it reads correctly
        // to nearby players.
        if (!AI.bHealingItemEquipped)
        {
            AI.bHealingItemEquipped = true;
            const bool bShieldPreview = Pawn->GetShield() < 50.f && AI.ShieldItemCount > 0;
            InventoryBehavior::EquipHealingItem(AI, bShieldPreview);
        }

        if (Now >= AI.ActionEndTime)
        {
            const float Shield = Pawn->GetShield();
            const bool bShield = Shield < 50.f && AI.ShieldItemCount > 0;
            const float Amount = InventoryBehavior::ConsumeHealingItem(AI, bShield);

            if (Amount > 0.f)
            {
                if (bShield)
                {
                    float MaxShield = Pawn->GetMaxShield();
                    if (MaxShield <= 0.f)
                        MaxShield = 100.f;
                    float NewShield = Shield + Amount;
                    if (NewShield > MaxShield)
                        NewShield = MaxShield;
                    Pawn->SetShield(NewShield);
                }
                else
                {
                    float MaxHealth = Pawn->GetMaxHealth();
                    if (MaxHealth <= 0.f)
                        MaxHealth = 100.f;
                    float NewHealth = Pawn->GetHealth() + Amount;
                    if (NewHealth > MaxHealth)
                        NewHealth = MaxHealth;
                    Pawn->SetHealth(NewHealth);
                }

                ReplicationBehavior::PushHealthShieldUpdate(Pawn);
                AIDebugLogger::Verbose("Combat", "%s healed (%s)", AI.Entity.DisplayName.c_str(), bShield ? "shield" : "health");
            }

            // Put the weapon back in hand.
            AI.bHealingItemEquipped = false;
            AI.EquippedRole = EPlayerAIWeaponRole::None; // force the re-equip
            InventoryBehavior::RefreshAndEquipBest(AI, false);

            AI.SetState(AI.CombatTarget ? EPlayerAIState::EngagingEnemy : EPlayerAIState::SearchingForLoot, "healing done");
        }
        break;
    }

    case EPlayerAIState::Retreating:
    {
        AI.CombatState = EPlayerAICombatState::Retreating;

        if (!AI.bHasMoveTarget)
        {
            FVector Loc = Pawn->K2_GetActorLocation();
            FVector Away = Loc - AI.LastKnownTargetLocation;
            Away.Z = 0;
            const double Len = Away.Magnitude();
            FVector Dir = Len > 1.0 ? Away / Len : FVector(1, 0, 0);

            NavigationBehavior::SetMoveTarget(AI, NavigationBehavior::RandomPointNear(Loc + Dir * 4500.0, 1200.f));
            break;
        }

        NavigationBehavior::CheckStuck(AI, Now);

        // Far enough: heal or resume.
        FVector Loc = Pawn->K2_GetActorLocation();

        if (Loc.GetDistanceTo(AI.LastKnownTargetLocation) > 5000.0 || Now - AI.TargetLastSeenTime > 8.f)
        {
            AI.CombatTarget = nullptr;

            if ((AI.HealingItemCount > 0 || AI.ShieldItemCount > 0) && PlayerAIRandChance(AI.Skill.HealingDiscipline))
            {
                AI.ActionEndTime = Now + PlayerAIConfig::HealDuration;
                AI.SetState(EPlayerAIState::Healing, "healing after retreat");
            }
            else
            {
                AI.SetState(EPlayerAIState::SearchingForLoot, "escaped the fight");
            }
        }
        break;
    }

    case EPlayerAIState::ThirdPartying:
    {
        auto Enemy = DetectEnemy(AI, World);

        if (Enemy)
        {
            AI.CombatTarget = Enemy;
            AI.TargetLastSeenTime = Now;
            AI.ReactionReadyTime = Now + AI.Skill.ReactionTimeSeconds * PlayerAIRandRange(0.6f, 1.1f);
            AI.SetState(EPlayerAIState::EngagingEnemy, "third party engage");
            break;
        }

        if (!AI.bHasMoveTarget)
            AI.SetState(EPlayerAIState::SearchingForEnemies, "third party over");
        else
            NavigationBehavior::CheckStuck(AI, Now);
        break;
    }

    default:
        break;
    }
}

void CombatBehavior::Tick(PlayerAIController& AI, float Now, float DeltaSeconds, FPlayerAIWorldSnapshot& World)
{
    // Release held native jumps (runs every tick for every AI; a jump left
    // pressed makes the pawn bunny-hop forever).
    if (AI.JumpStopTime > 0.f && Now >= AI.JumpStopTime)
    {
        AI.JumpStopTime = 0.f;

        if (auto JumpPawn = AI.GetPawn())
            JumpPawn->StopJumping();
    }

    if (AI.GetState() != EPlayerAIState::EngagingEnemy)
    {
        // Never leave a trigger held or aim pitch applied outside combat.
        if (AI.bNativeFiring)
            NativePlayerAIBackend::StopFire(AI);

        auto IdlePawn = AI.GetPawn();

        if (IdlePawn && IdlePawn->HasRemoteViewPitch() && IdlePawn->RemoteViewPitch != 0)
            IdlePawn->RemoteViewPitch = 0;

        return;
    }

    auto Pawn = AI.GetPawn();

    if (!Pawn || !IsValidEnemy(AI, AI.CombatTarget))
    {
        if (AI.Entity.bNativeBacked && AI.bNativeFiring)
            NativePlayerAIBackend::StopFire(AI);

        return;
    }

    auto EnemyPawn = AI.CombatTarget->Pawn;

    if (!EnemyPawn)
        return;

    FVector Loc = Pawn->K2_GetActorLocation();
    FVector EnemyLoc = EnemyPawn->K2_GetActorLocation();

    // Face the target while fighting.
    FVector Dir = EnemyLoc - Loc;
    const double Dist = Dir.Magnitude();

    if (Dist > 1.0)
    {
        Dir = Dir / Dist;
        FRotator FaceRot{};
        FaceRot.Yaw = atan2(Dir.Y, Dir.X) * PLAYERAI_RAD_TO_DEG;
        Pawn->K2_SetActorRotation(FaceRot, false);

        if (!AI.Entity.bNativeBacked && AI.Entity.PC)
            AI.Entity.PC->SetControlRotation(FaceRot);
    }

    // Replicated aim pitch so the AI visibly looks up/down at its target.
    if (Pawn->HasRemoteViewPitch())
    {
        FVector FlatTo = EnemyLoc - Loc;
        const double Flat2D = sqrt(FlatTo.X * FlatTo.X + FlatTo.Y * FlatTo.Y);
        double PitchDeg = atan2((double)EnemyLoc.Z - ((double)Loc.Z + 60.0), Flat2D > 1.0 ? Flat2D : 1.0) * PLAYERAI_RAD_TO_DEG;

        if (PitchDeg < 0.0)
            PitchDeg += 360.0;

        Pawn->RemoteViewPitch = (uint8)(PitchDeg * 256.0 / 360.0);
    }

    // Native backend: aim with skill-based error and pull the REAL trigger.
    // Bullets, damage, kill feed and kill credit are all the engine's own -
    // no simulated damage is applied at all.
    if (AI.Entity.bNativeBacked)
    {
        if (Now < AI.ReactionReadyTime)
            return;

        if (Dist > AI.Skill.EngageRange)
        {
            NativePlayerAIBackend::StopFire(AI);
            return;
        }

        FVector AimPoint = EnemyLoc;
        const float AimError = (1.f - AI.Skill.AimAccuracy) * (float)(60.0 + Dist * 0.035);
        AimPoint.X += PlayerAIRandRange(-AimError, AimError);
        AimPoint.Y += PlayerAIRandRange(-AimError, AimError);
        AimPoint.Z += PlayerAIRandRange(-AimError * 0.5f, AimError * 0.75f);
        NativePlayerAIBackend::SetFocalPoint(AI, AimPoint);

        if (Now >= AI.NextShotTime)
        {
            if (AI.bNativeFiring)
            {
                // End the burst, breathe, repeat.
                NativePlayerAIBackend::StopFire(AI);
                AI.NextShotTime = Now + PlayerAIFireInterval(AI.EquippedRole) * PlayerAIRandRange(0.8f, 1.5f);
            }
            else
            {
                NativePlayerAIBackend::StartFire(AI);
                AI.NextShotTime = Now + PlayerAIRandRange(0.25f, 0.9f);
            }
        }
        return;
    }

    // ---- Simulated backend fire ------------------------------------------
    // Release the previous trigger tap and evaluate the real-fire probe:
    // when trigger fire consumes ammo, the engine's bullets are real - and
    // simulated damage turns itself off for the session.
    if (AI.bNativeFiring && Now >= AI.FireTapEndTime)
    {
        NativePlayerAIBackend::StopFire(AI);

        if (GRealFireWorks == -1 && AI.ProbeAmmoAtTap >= 0)
        {
            const int AmmoNow = PlayerAIGetEquippedLoadedAmmo(AI);

            if (AmmoNow >= 0 && AmmoNow < AI.ProbeAmmoAtTap)
            {
                GRealFireWorks = 1;
                AIDebugLogger::Log("Combat", "real weapon fire works on this version - simulated damage disabled");
            }
            else if (++GRealFireProbes >= 6)
            {
                GRealFireWorks = 0;
                AIDebugLogger::MissingFeature("RealWeaponFireForPlayerAI",
                    "trigger fire is cosmetic on this version - simulated damage stays authoritative");
            }

            AI.ProbeAmmoAtTap = -1;
        }
    }

    // Ability-fire probe: did the last ability shot consume real ammo?
    if (GRealFireWorks == 0 && GAbilityFireWorks == -1 && AI.ProbeAmmoAtTap >= 0 && !AI.bNativeFiring)
    {
        const int AmmoNow = PlayerAIGetEquippedLoadedAmmo(AI);

        if (AmmoNow >= 0 && AmmoNow < AI.ProbeAmmoAtTap)
        {
            GAbilityFireWorks = 1;
            AIDebugLogger::Log("Combat", "ability weapon fire works on this version - simulated damage disabled");
        }
        else if (++GAbilityFireProbes >= 6)
        {
            GAbilityFireWorks = 0;
            AIDebugLogger::MissingFeature("AbilityWeaponFireForPlayerAI",
                "ability fire is cosmetic on this version - simulated damage stays authoritative");
        }

        AI.ProbeAmmoAtTap = -1;
    }

    if (Now < AI.ReactionReadyTime || Now < AI.NextShotTime)
        return;

    if (Dist > AI.Skill.EngageRange)
        return;

    // Shots only apply damage at plausible weapon ranges; outside them the
    // AI keeps closing the distance instead of hitting from nowhere.
    const double MaxDamageRange = PlayerAIMaxDamageRange(AI.EquippedRole);

    if (Dist > MaxDamageRange)
        return;

    if (AI.MagazineRemaining <= 0)
    {
        AI.ActionEndTime = Now + PlayerAIConfig::ReloadDuration;
        AI.SetState(EPlayerAIState::Reloading, "magazine empty");
        return;
    }

    // Shot cadence with skill based accuracy: distance and randomness keep
    // the aim honest (never perfect outside the Testing profile).
    AI.NextShotTime = Now + PlayerAIFireInterval(AI.EquippedRole) * PlayerAIRandRange(0.9f, 1.35f);
    AI.MagazineRemaining--;

    if (GRealFireWorks != 0)
    {
        // Pull the real trigger for this shot (muzzle flash / audio /
        // possibly real bullets - detected by the ammo probe above).
        if (GRealFireWorks == -1)
            AI.ProbeAmmoAtTap = PlayerAIGetEquippedLoadedAmmo(AI);

        NativePlayerAIBackend::StartFire(AI);
        AI.FireTapEndTime = Now + 0.12f;

        // Real bullets carry the damage on versions where they work.
        if (GRealFireWorks == 1)
            return;
    }
    else if (GAbilityFireWorks != 0)
    {
        // Trigger fire is dead here: fire the weapon's primary ability
        // server-side instead (real bullets, sound, tracers and native
        // damage/kill credit where the ability path works).
        if (GAbilityFireWorks == -1 && AI.ProbeAmmoAtTap < 0)
            AI.ProbeAmmoAtTap = PlayerAIGetEquippedLoadedAmmo(AI);

        VersionFeatureAdapter::TryFireEquippedWeapon(AI.Entity.PC, Pawn);

        // Real bullets carry the damage on versions where they work.
        if (GAbilityFireWorks == 1)
            return;
    }

    float HitChance = AI.Skill.AimAccuracy;

    if (AI.Skill.Profile != EPlayerAISkillProfile::Testing)
    {
        const float DistFactor = (float)(Dist / MaxDamageRange); // 0 close .. 1 far
        HitChance *= 1.15f - 0.65f * DistFactor;

        if (HitChance > 0.95f)
            HitChance = 0.95f;
    }

    if (!PlayerAIRandChance(HitChance))
        return; // simulated miss

    float Damage = PlayerAIWeaponDamage(AI.EquippedRole);

    // Distance falloff: far hits chip instead of melting.
    if (AI.Skill.Profile != EPlayerAISkillProfile::Testing)
        Damage *= (float)(1.0 - 0.45 * (Dist / MaxDamageRange));

    if (PlayerAIRandChance(0.12f))
        Damage *= 1.6f; // occasional headshot

    const bool bKilled = DamageBehavior::ApplyWeaponDamage(AI, AI.CombatTarget, Damage);

    if (bKilled)
    {
        EliminationBehavior::OnEliminatedOther(AI, AI.CombatTarget);
        AI.CombatTarget = nullptr;
        AI.CombatState = EPlayerAICombatState::Searching;
        AI.SetState(EPlayerAIState::ManagingInventory, "eliminated the enemy");
    }
}

// ============================================================================
// DamageBehavior
// ============================================================================

bool DamageBehavior::ApplyWeaponDamage(PlayerAIController& Attacker, AFortPlayerControllerAthena* VictimPC, float Damage)
{
    if (!VictimPC)
        return false;

    auto VictimPawn = VictimPC->Pawn; // AController base property - safe for any controller kind

    if (!VictimPawn || VictimPawn->GetHealth() <= 0.f)
        return false;

    // Aggro + last damage source bookkeeping when the victim is a PlayerAI.
    if (auto VictimAI = PlayerAIManager::FindByController(VictimPC))
    {
        VictimAI->LastDamagerPC = Attacker.Entity.PC;
        VictimAI->LastDamageTime = VersionFeatureAdapter::GetTimeSeconds();

        // Fight back when idle/looting - never while mid-drop.
        if (VictimAI->GetState() != EPlayerAIState::EngagingEnemy &&
            VictimAI->GetState() != EPlayerAIState::Retreating &&
            !PlayerAIIsAirState(VictimAI->GetState()) &&
            VictimAI->IsAlive() && Attacker.Entity.PC)
        {
            VictimAI->CombatTarget = Attacker.Entity.PC;
            VictimAI->TargetLastSeenTime = VictimAI->LastDamageTime;
            VictimAI->ReactionReadyTime = VictimAI->LastDamageTime + VictimAI->Skill.ReactionTimeSeconds;
            VictimAI->SetState(EPlayerAIState::EngagingEnemy, "damaged - fighting back");
        }
    }

    const float Shield = VictimPawn->GetShield();
    const float Health = VictimPawn->GetHealth();
    float Remaining = Damage;

    if (Shield > 0.f)
    {
        float NewShield = Shield - Remaining;

        if (NewShield < 0.f)
            NewShield = 0.f;

        Remaining -= (Shield - NewShield);
        VictimPawn->SetShield(NewShield);
    }

    bool bFatal = false;

    if (Remaining > 0.f)
    {
        if (Health <= Remaining + 0.01f)
        {
            bFatal = true;

            // NOTE: on versions with down-but-not-out squads, the native
            // ForceKill flow applies the appropriate DBNO/elimination result.
            // TODO: connect this to the Magnesium damage system for explicit
            //       DBNO downing instead of direct elimination if desired.
            auto Weapon = VictimPawn && Attacker.GetPawn() && Attacker.GetPawn()->HasCurrentWeapon() ? Attacker.GetPawn()->CurrentWeapon : nullptr;
            VersionFeatureAdapter::KillPawn(VictimPawn, Attacker.Entity.PC, Weapon);
        }
        else
        {
            VictimPawn->SetHealth(Health - Remaining);
        }
    }

    ReplicationBehavior::PushHealthShieldUpdate(VictimPawn);
    return bFatal;
}

bool DamageBehavior::ApplyEnvironmentalDamage(PlayerAIController& AI, float Damage)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return false;

    const float Health = Pawn->GetHealth();

    if (Health <= 0.f)
        return false;

    AIDebugLogger::Verbose("Damage", "%s takes %.0f storm damage (health %.0f)", AI.Entity.DisplayName.c_str(), Damage, Health);

    if (Health <= Damage)
    {
        // Storm/zone deaths give no kill credit (native rules unchanged).
        VersionFeatureAdapter::KillPawn(Pawn, nullptr, nullptr);
        return true;
    }

    Pawn->SetHealth(Health - Damage);
    ReplicationBehavior::PushHealthShieldUpdate(Pawn);
    return false;
}

void DamageBehavior::DetectDeath(PlayerAIController& AI, float Now)
{
    if (AI.bDeathHandled)
        return;

    auto Pawn = AI.GetPawn();

    if (!Pawn)
    {
        // Pawn removed while in a ground state = eliminated + cleaned up by
        // the native pipeline.
        AI.bDeathHandled = true;
        AI.SetState(EPlayerAIState::Dead, "pawn removed");
        EliminationBehavior::OnEliminated(AI, Now);
        return;
    }

    // DBNO support: follow the native down-but-not-out flow when active.
    if (Pawn->IsDBNO())
    {
        if (AI.GetState() != EPlayerAIState::DownedOrDisabled)
        {
            AI.DBNOSince = Now;
            NavigationBehavior::ClearMoveTarget(AI);
            AI.SetState(EPlayerAIState::DownedOrDisabled, "downed");
        }
        else if (Now - AI.DBNOSince > 35.f)
        {
            // Bleedout fallback if nothing finished the AI (keeps matches
            // moving on versions where native bleedout skips server AI).
            AIDebugLogger::Verbose("Damage", "%s bleedout fallback", AI.Entity.DisplayName.c_str());
            VersionFeatureAdapter::KillPawn(Pawn, AI.LastDamagerPC, nullptr);
        }
        return;
    }

    if (AI.GetState() == EPlayerAIState::DownedOrDisabled)
    {
        // Revived (or DBNO state ended without death).
        if (Pawn->GetHealth() > 0.f)
        {
            AI.SetState(EPlayerAIState::SearchingForLoot, "recovered from DBNO");
            return;
        }
    }

    if (Pawn->GetHealth() <= 0.f)
    {
        AI.bDeathHandled = true;
        AI.SetState(EPlayerAIState::Dead, "health reached zero");
        EliminationBehavior::OnEliminated(AI, Now);
    }
}

// ============================================================================
// EliminationBehavior
// ============================================================================

void EliminationBehavior::OnEliminated(PlayerAIController& AI, float Now)
{
    PlayerAIManager::EliminatedCount++;

    AIDebugLogger::Log("Elimination", "AIPlayer %s was eliminated (alive PlayerAIs: %d)",
        AI.Entity.DisplayName.c_str(), PlayerAIManager::GetAliveCount());

    // Kill credit itself flows through the existing Magnesium elimination
    // pipeline (ClientOnPawnDied): kill feed, killer stats, placement and
    // win-condition checks all ran natively for this death.
    auto Counts = VictoryConditionBehavior::UpdateAliveCounts(PlayerAIManager::GetWorld());

    AIDebugLogger::Log("AliveCount", "Total=%d Real=%d PlayerAI=%d",
        Counts.TotalAlivePlayers, Counts.AliveRealPlayers, Counts.AlivePlayerAIs);
}

void EliminationBehavior::OnEliminatedOther(PlayerAIController& AI, AFortPlayerControllerAthena* Victim)
{
    const char* VictimKind = PlayerAIManager::IsPlayerAI(Victim) ? "PlayerAI" : "player";

    AIDebugLogger::Log("Elimination", "AIPlayer %s eliminated a %s", AI.Entity.DisplayName.c_str(), VictimKind);
}

// ============================================================================
// VictoryConditionBehavior
// ============================================================================

VictoryConditionBehavior::FAliveCounts VictoryConditionBehavior::UpdateAliveCounts(FPlayerAIWorldSnapshot& World)
{
    FAliveCounts Counts{};

    if (World.GameMode)
    {
        auto CountList = [&](TArray<AActor*>& List)
            {
                for (auto& Uncasted : List)
                {
                    auto PC = (AFortPlayerControllerAthena*)Uncasted;

                    if (!PC)
                        continue;

                    Counts.TotalAlivePlayers++;

                    if (PlayerAIManager::IsPlayerAI(PC))
                        Counts.AlivePlayerAIs++;
                    else
                        Counts.AliveRealPlayers++;
                }
            };

        CountList(World.GameMode->AlivePlayers);

        if (World.GameMode->HasAliveBots())
            CountList(World.GameMode->AliveBots);
    }

    LastCounts = Counts;
    return Counts;
}
