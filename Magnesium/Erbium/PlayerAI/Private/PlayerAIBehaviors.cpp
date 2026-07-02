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
#include "../../../FortniteGame/Public/FortLootPackage.h"
#include <unordered_map>
#include <algorithm>

static constexpr double PLAYERAI_RAD_TO_DEG = 57.29577951308232;
static constexpr float PLAYERAI_PAWN_HALF_HEIGHT = 90.f;

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

    FVector Dir = To / Dist;

    float Speed = SpeedOverride > 0.f ? SpeedOverride : AI.Skill.MoveSpeed;
    // Movement quality: lower skill wobbles the speed a little more.
    Speed *= 0.90f + 0.20f * AI.Skill.MovementQuality;

    // Periodic ground snapping keeps the AI walking on terrain without
    // relying on any single version specific navigation system.
    if (Now >= AI.NextGroundSnapTime)
    {
        AI.NextGroundSnapTime = Now + PlayerAIConfig::GroundSnapInterval;

        FVector Ahead = Loc + Dir * 250.0;
        bool bFound = false;
        FVector Ground = VersionFeatureAdapter::FindGroundLocation(Ahead, bFound, Pawn);

        if (bFound)
            AI.CachedGroundZ = (float)Ground.Z;
    }

    FVector NewLoc = Loc + Dir * (Speed * DeltaSeconds);

    const double TargetZ = (double)AI.CachedGroundZ + PLAYERAI_PAWN_HALF_HEIGHT;
    const double DeltaZ = TargetZ - Loc.Z;

    if (fabs(DeltaZ) < 1200.0)
    {
        double Alpha = 10.0 * DeltaSeconds;
        if (Alpha > 1.0)
            Alpha = 1.0;
        NewLoc.Z = Loc.Z + DeltaZ * Alpha;
    }
    else
    {
        // Large height difference (cliff/fall) - keep the current Z and let
        // gravity / next ground snap sort it out.
        NewLoc.Z = Loc.Z;
    }

    FRotator FaceRot{};
    FaceRot.Yaw = atan2(Dir.Y, Dir.X) * PLAYERAI_RAD_TO_DEG;
    Pawn->K2_SetActorRotation(FaceRot, false);

    Pawn->K2_SetActorLocation(NewLoc, false, nullptr, true);

    // Velocity is replicated and drives movement animations on clients.
    if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
    {
        FVector Vel = Dir * Speed;
        Vel.Z = 0;
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

    if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
    {
        auto CurVel = Pawn->CharacterMovement->Velocity;

        double HorizSpeed = Dist > 6000.0 ? 1500.0 : (Dist * 0.25);
        FVector NewVel = Dir * HorizSpeed;
        NewVel.Z = CurVel.Z; // let the native skydive/glide handle fall speed

        Pawn->CharacterMovement->Velocity = NewVel;
    }

    FRotator FaceRot{};
    FaceRot.Yaw = atan2(Dir.Y, Dir.X) * PLAYERAI_RAD_TO_DEG;
    Pawn->K2_SetActorRotation(FaceRot, false);
}

void NavigationBehavior::TryJump(PlayerAIController& AI)
{
    static bool bJumpDisabled = false;

    auto Pawn = AI.GetPawn();

    if (!Pawn || bJumpDisabled)
        return;

    // Standard parameterless Jump, invoked with a sized zeroed parameter
    // buffer inside a fault guard. Jumping is flavor: when unsupported the
    // AI simply keeps walking.
    auto JumpFn = Pawn->GetFunction("Jump");

    if (!JumpFn)
    {
        bJumpDisabled = true;
        AIDebugLogger::MissingFeature("JumpForPlayerAI", "no Jump function on this version - PlayerAI keeps walking");
        return;
    }

    if (!VersionFeatureAdapter::SafeCallNoArgs(Pawn, JumpFn))
    {
        bJumpDisabled = true;
        AIDebugLogger::MissingFeature("JumpForPlayerAI", "native jump faulted and was disabled - PlayerAI keeps walking");
    }
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
                TryJump(AI);
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
                // the target and force a replication update.
                bool bFound = false;
                FVector Ground = VersionFeatureAdapter::FindGroundLocation(AI.MoveTarget, bFound, Pawn);
                FVector SafePoint = bFound ? Ground : Loc;
                SafePoint.Z += 100.f;

                AIDebugLogger::Log("Navigation", "%s teleport recovery to (%.0f, %.0f, %.0f) after repeated stuck detection",
                    AI.Entity.DisplayName.c_str(), SafePoint.X, SafePoint.Y, SafePoint.Z);

                Pawn->K2_TeleportTo(SafePoint, Pawn->K2_GetActorRotation(), false, true);
                ReplicationBehavior::PushTeleportUpdate(Pawn);
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
            break;

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
            NavigationBehavior::TryJump(AI);

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
    auto PC = AI.Entity.PC;

    if (!PC)
        return;

    switch (AI.GetState())
    {
    case EPlayerAIState::WaitingForTransport:
    {
        // The native flow may have put us into the aircraft already.
        if (VersionFeatureAdapter::IsInAircraft(PC))
        {
            AI.bEnteredTransport = true;
            AI.SetState(EPlayerAIState::InTransport, "native aircraft entry");
            break;
        }

        // Remove the leftover warmup pawn (real player pawns are removed by
        // the native transport flow; ours may linger on versions where that
        // flow ignores server side players).
        auto Pawn = AI.GetPawn();

        if (Pawn)
        {
            Pawn->K2_DestroyActor();

            if (PC->HasMyFortPawn())
                PC->MyFortPawn = nullptr;
        }

        if (VersionFeatureAdapter::EnterAircraft(PC))
        {
            AI.bEnteredTransport = true;
            AI.SetState(EPlayerAIState::InTransport, "entered aircraft");
        }
        else
        {
            // Safe fallback: simulated transport seat. The AI joins the
            // match at its landing target when its jump time arrives.
            AI.bVirtualTransport = true;
            AI.bEnteredTransport = true;
            AIDebugLogger::MissingFeature("AircraftEntryForPlayerAI", "using simulated transport seat + landing placement");
            AI.SetState(EPlayerAIState::InTransport, "simulated transport seat");
        }
        break;
    }

    case EPlayerAIState::InTransport:
    {
        // Thank the driver only when the version supports it.
        if (!AI.bThankedDriver && Now >= AI.ThankDriverTime)
        {
            AI.bThankedDriver = true;

            if (!AI.bVirtualTransport && PlayerAIRandChance(PlayerAIConfig::ThankDriverChance))
            {
                if (VersionFeatureAdapter::ThankDriver(PC))
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
            }
        }

        if (bWantsJump)
        {
            AI.JumpedAtTime = Now;

            if (!AI.bVirtualTransport && VersionFeatureAdapter::JumpFromAircraft(PC))
            {
                AI.bJumpedFromTransport = true;
                AI.SetState(EPlayerAIState::JumpingFromTransport, "jumped from transport");
            }
            else
            {
                // Fallback: place the AI directly at its landing spot.
                AI.bJumpedFromTransport = true;
                AI.SetState(EPlayerAIState::JumpingFromTransport, "simulated jump");
            }
        }
        break;
    }

    case EPlayerAIState::JumpingFromTransport:
    {
        auto Pawn = AI.GetPawn();

        if (Pawn && !AI.bVirtualTransport)
        {
            // Native jump produced a skydiving pawn.
            AI.SetState(EPlayerAIState::Gliding, "skydiving");
            break;
        }

        // Simulated seat or the native jump did not produce a pawn in time:
        // spawn the pawn at the landing target (logged fallback).
        if (AI.bVirtualTransport || Now - AI.JumpedAtTime > 3.f)
        {
            auto NewPawn = MagnesiumPlayerAISpawner::SpawnPawnAt(AI, AI.LandingTarget, true);

            if (NewPawn)
            {
                AI.SetState(EPlayerAIState::Landing, "fallback landing placement");
            }
            else if (Now - AI.JumpedAtTime > 10.f)
            {
                AIDebugLogger::Error("Transport", "%s could not enter the match - retrying landing placement", AI.Entity.DisplayName.c_str());
                AI.JumpedAtTime = Now; // retry loop, never crash
            }
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

    if (!Pawn)
    {
        // Pawn vanished mid-air (e.g. version quirk): use the fallback
        // landing placement instead of freezing.
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

    FVector Loc = Pawn->K2_GetActorLocation();

    bool bStillAirborne = false;

    if (Pawn->HasbIsSkydiving())
        bStillAirborne = Pawn->bIsSkydiving;
    else
    {
        bool bFound = false;
        FVector Ground = VersionFeatureAdapter::FindGroundLocation(Loc, bFound, Pawn);
        bStillAirborne = bFound && (Loc.Z - Ground.Z) > 500.0;
    }

    if (!bStillAirborne)
    {
        AIDebugLogger::Verbose("Landing", "%s landed at (%.0f, %.0f)", AI.Entity.DisplayName.c_str(), Loc.X, Loc.Y);
        AI.CachedGroundZ = (float)Loc.Z - PLAYERAI_PAWN_HALF_HEIGHT;
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
    auto PC = AI.Entity.PC;

    if (!PC || !Pickup)
        return false;

    if (Pickup->HasbPickedUp() && Pickup->bPickedUp)
        return false;

    auto Inventory = PC->WorldInventory;

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
    auto PC = AI.Entity.PC;

    if (!PC || !PC->WorldInventory)
        return;

    auto& Entries = PC->WorldInventory->Inventory.ReplicatedEntries;

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

    PC->ServerExecuteInventoryItem(ToEquip->ItemGuid);
    PC->ClientEquipItem(ToEquip->ItemGuid, true);

    AI.EquippedItemGuid = ToEquip->ItemGuid;
    AI.EquippedRole = ClassifyItem(ToEquip->ItemDefinition);
    AI.MagazineRemaining = PlayerAIConfig::MagazineSizeDefault;

    AIDebugLogger::Verbose("Inventory", "%s equipped %s", AI.Entity.DisplayName.c_str(), ToEquip->ItemDefinition->Name.ToString().c_str());
}

float InventoryBehavior::ConsumeHealingItem(PlayerAIController& AI, bool bShield)
{
    auto PC = AI.Entity.PC;

    if (!PC || !PC->WorldInventory)
        return 0.f;

    auto& Entries = PC->WorldInventory->Inventory.ReplicatedEntries;

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
            PC->WorldInventory->UpdateEntry(*Entry);
        }
        else
        {
            PC->WorldInventory->Remove(Entry->ItemGuid);
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

    auto EnemyPawn = Enemy->HasMyFortPawn() && Enemy->MyFortPawn ? Enemy->MyFortPawn : Enemy->Pawn;

    if (!EnemyPawn || EnemyPawn->GetHealth() <= 0.f)
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
    auto Pawn = AI.GetPawn();

    if (!Pawn || !World.GameMode)
        return nullptr;

    FVector Loc = Pawn->K2_GetActorLocation();

    AFortPlayerControllerAthena* Best = nullptr;
    double BestDist = AI.Skill.DetectionRange;

    // ONE shared detection path for real human players and other PlayerAI
    // players: everyone in the alive players list is a potential enemy.
    for (auto& Uncasted : World.GameMode->AlivePlayers)
    {
        auto Enemy = (AFortPlayerControllerAthena*)Uncasted;

        if (!IsValidEnemy(AI, Enemy))
            continue;

        auto EnemyPawn = Enemy->HasMyFortPawn() && Enemy->MyFortPawn ? Enemy->MyFortPawn : Enemy->Pawn;
        const double Dist = Loc.GetDistanceTo(EnemyPawn->K2_GetActorLocation());

        if (Dist >= BestDist)
            continue;

        // Distance based spotting: far targets are noticed less reliably
        // (stands in for visibility/sound cues on versions without cheap
        // trace support).
        const float SpotChance = Dist < 4000.0 ? 1.f : (float)(1.0 - (Dist / AI.Skill.DetectionRange) * 0.6);

        if (!PlayerAIRandChance(SpotChance))
            continue;

        BestDist = Dist;
        Best = Enemy;
    }

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

        auto EnemyPawn = AI.CombatTarget->HasMyFortPawn() && AI.CombatTarget->MyFortPawn ? AI.CombatTarget->MyFortPawn : AI.CombatTarget->Pawn;

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
    if (AI.GetState() != EPlayerAIState::EngagingEnemy)
        return;

    auto Pawn = AI.GetPawn();

    if (!Pawn || !IsValidEnemy(AI, AI.CombatTarget))
        return;

    auto EnemyPawn = AI.CombatTarget->HasMyFortPawn() && AI.CombatTarget->MyFortPawn ? AI.CombatTarget->MyFortPawn : AI.CombatTarget->Pawn;

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
    }

    if (Now < AI.ReactionReadyTime || Now < AI.NextShotTime)
        return;

    if (Dist > AI.Skill.EngageRange)
        return;

    if (AI.MagazineRemaining <= 0)
    {
        AI.ActionEndTime = Now + PlayerAIConfig::ReloadDuration;
        AI.SetState(EPlayerAIState::Reloading, "magazine empty");
        return;
    }

    // Simulated shot with skill based accuracy: distance and randomness
    // keep the aim honest (never perfect outside the Testing profile).
    AI.NextShotTime = Now + PlayerAIFireInterval(AI.EquippedRole) * PlayerAIRandRange(0.9f, 1.35f);
    AI.MagazineRemaining--;

    float HitChance = AI.Skill.AimAccuracy;

    if (AI.Skill.Profile != EPlayerAISkillProfile::Testing)
    {
        const float DistFactor = (float)(Dist / AI.Skill.EngageRange); // 0 close .. 1 far
        HitChance *= 1.15f - 0.65f * DistFactor;

        if (HitChance > 0.95f)
            HitChance = 0.95f;
    }

    if (!PlayerAIRandChance(HitChance))
        return; // simulated miss

    float Damage = PlayerAIWeaponDamage(AI.EquippedRole);

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

    auto VictimPawn = VictimPC->HasMyFortPawn() && VictimPC->MyFortPawn ? VictimPC->MyFortPawn : VictimPC->Pawn;

    if (!VictimPawn || VictimPawn->GetHealth() <= 0.f)
        return false;

    // Aggro + last damage source bookkeeping when the victim is a PlayerAI.
    if (auto VictimAI = PlayerAIManager::FindByController(VictimPC))
    {
        VictimAI->LastDamagerPC = Attacker.Entity.PC;
        VictimAI->LastDamageTime = VersionFeatureAdapter::GetTimeSeconds();

        // Fight back when idle/looting.
        if (VictimAI->GetState() != EPlayerAIState::EngagingEnemy &&
            VictimAI->GetState() != EPlayerAIState::Retreating &&
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
        for (auto& Uncasted : World.GameMode->AlivePlayers)
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
    }

    LastCounts = Counts;
    return Counts;
}
