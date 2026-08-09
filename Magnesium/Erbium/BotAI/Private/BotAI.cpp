#include "pch.h"
// ============================================================================
// Magnesium - BotAI implementation
//
// One entity per spawnbot controller, one small state machine per entity:
//
//   Wander    - stroll to a random nearby point, re-pick on arrival/timeout
//   SeekZone  - rotate into the safe circle (wins over everything on foot)
//   Falling   - skydiving/gliding: steer horizontally toward the landing spot
//   InBus     - aboard an aircraft: wait for the drop window, then jump
//
// Stuck recovery escalates inside Wander/SeekZone rather than being its own
// state: jump, sidestep, re-pick the goal, and finally a short traced hop.
//
// Local control, and why it is the whole problem
//
// UCharacterMovementComponent only runs ControlledCharacterMove - the path
// that consumes AddMovementInput, applies acceleration, and picks walking vs
// falling vs swimming - when APawn::IsLocallyControlled() is true. That asks
// the possessing controller, and APlayerController answers "no" unless it has
// a ULocalPlayer. A spawnbot controller is an AFortPlayerControllerAthena with
// no UNetConnection and no ULocalPlayer, so the component never ticks a move
// for its pawn: neither AddMovementInput nor a Velocity write does anything.
// (Diagnostics on 12.61 confirmed this: localCtrl=0, accel=0, moved=0, with
// the input call itself packing correctly and the component in MOVE_Walking.)
//
// Note that the base AController implementation would have said "yes" - the
// controller's roles are Authority/None, which is exactly the local-authority
// case. Only the APlayerController override, and its missing UPlayer, stands
// in the way.
//
// So AcquireLocalControl gives the bot controller a ULocalPlayer of its own.
// It is never registered with the game instance, so nothing else in the engine
// treats it as the host player; it exists only to answer that one question.
// When it works, movement is completely native - real walk and run speeds,
// real animation, real collision, real swimming, no transform writes at all.
//
// When it does not work, the swept driver below takes over: K2_SetActorLocation
// with bSweep moves the real collision capsule and gravity is a swept drop onto
// real geometry. That keeps bots working on any build, but it is a simulation -
// it cannot swim. Each bot picks its own path from what the engine actually
// reports, and logs which one it took.
// ============================================================================
#include "../Public/BotAI.h"
#include "../../Support/Public/AIDebugLogger.h"
#include "../../Support/Public/VersionFeatureAdapter.h"
#include "../../../Engine/Public/AbilitySystemComponent.h"
#include "../../Public/GUI.h"
#include "../../../FortniteGame/Public/FortGameMode.h"
#include "../../../FortniteGame/Public/FortGameStateAthena.h"
#include "../../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../../FortniteGame/Public/FortPlayerPawnAthena.h"
#include "../../../FortniteGame/Public/FortPlayerStateAthena.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

std::atomic_bool BotAISettings::bEnabled{ false };
std::atomic_bool BotAISettings::bSeekSafeZone{ true };
std::atomic_bool BotAISettings::bIdleFlourishes{ true };
std::atomic_bool BotAISettings::bNativeMovement{ true };
std::atomic_bool BotAISettings::bMovementDiagnostics{ false };

namespace
{

// How a bot's pawn is actually being moved. Resolved per bot from what the
// engine reports, never assumed.
enum class EDriveMode : uint8_t
{
    Undecided,
    Native, // character movement runs: AddMovementInput and nothing else
    Swept,  // character movement will not run: BotAI moves the capsule
};

enum class EBotAIState : uint8_t
{
    Spawning,
    Wander,
    SeekZone,
    Falling,
    InBus,
};

const char* StateToString(EBotAIState State)
{
    switch (State)
    {
    case EBotAIState::Spawning: return "Spawning";
    case EBotAIState::Wander:   return "Wander";
    case EBotAIState::SeekZone: return "SeekZone";
    case EBotAIState::Falling:  return "Falling";
    case EBotAIState::InBus:    return "InBus";
    }
    return "Unknown";
}

// A goal closer than this counts as reached. Roughly one capsule plus the
// slop native acceleration overshoots by at running speed.
constexpr double GoalAcceptRadius = 180.0;

// Sprint rather than walk once the remaining trip is worth it. Below this a
// bot strolls, which keeps short repositioning from looking frantic.
constexpr double SprintDistance = 1500.0;

// Fortnite's own default ground speeds. The swept walker has to supply a
// speed, so it uses the ones the game would have used.
constexpr float WalkSpeed = 460.f;
constexpr float SprintSpeed = 610.f;

// Wander hops. Short enough that a bot visibly explores rather than beelining
// across the island, long enough that it does not look like it is pacing.
constexpr float WanderMinDistance = 800.f;
constexpr float WanderMaxDistance = 4500.f;

// A goal is abandoned after this long even if it was never reached, so one
// unreachable point can never park a bot for the rest of the match.
constexpr float GoalTimeoutSeconds = 22.f;

struct FBotEntity
{
    AFortPlayerControllerAthena* PC = nullptr;
    AFortPlayerPawnAthena* Pawn = nullptr;

    EBotAIState State = EBotAIState::Spawning;
    float StateEnterTime = 0.f;

    FVector Goal{};
    bool bHasGoal = false;
    float GoalSetTime = 0.f;

    // Think work (zone evaluation, goal selection) is staggered across bots
    // so a full lobby never re-plans in the same frame.
    float NextThinkTime = 0.f;

    // Stuck detection.
    FVector ProgressLocation{};
    float LastProgressTime = 0.f;
    int StuckEscalation = 0;
    float DetourUntil = 0.f;
    FVector DetourGoal{};

    // Idle flourishes.
    float NextJumpTime = 0.f;
    float PauseUntil = 0.f;

    // ACharacter::Jump only latches bPressedJump. Without a matching
    // StopJumping the pawn re-jumps every time it touches the ground.
    float ReleaseJumpTime = 0.f;
    bool bJumpHeld = false;

    // Native sprint ability.
    bool bSprintRequested = false;
    float NextSprintAttemptTime = 0.f;

    EDriveMode DriveMode = EDriveMode::Undecided;
    int LocalControlAttempts = 0;

    // Swept walking.
    float VerticalVelocity = 0.f;
    bool bGrounded = false;

    // Movement diagnostics.
    float NextDiagnosticTime = 0.f;
    FVector DiagnosticLocation{};

    // Transport.
    FVector LandingTarget{};
    bool bHasLandingTarget = false;
    float NextJumpAttemptTime = 0.f;
};

std::unordered_map<AFortPlayerControllerAthena*, FBotEntity> GBots;
const void* GWorldToken = nullptr;
float GNextDiscoveryTime = 0.f;
bool GHooksRegistered = false;
char GStatusLine[160] = "Bot AI: idle";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

double Distance2D(const FVector& A, const FVector& B)
{
    const double DX = A.X - B.X;
    const double DY = A.Y - B.Y;
    return std::sqrt(DX * DX + DY * DY);
}

// FVector's assignment operators take non-const references, so copying a
// const FVector& into an existing vector needs an explicit temporary.
FVector Copy(const FVector& V)
{
    return FVector(V.X, V.Y, V.Z);
}

bool IsFinite(const FVector& V)
{
    return std::isfinite((double)V.X) && std::isfinite((double)V.Y) &&
        std::isfinite((double)V.Z);
}

float NormalizeYaw(float Yaw)
{
    while (Yaw > 180.f)
        Yaw -= 360.f;
    while (Yaw < -180.f)
        Yaw += 360.f;
    return Yaw;
}

// ---------------------------------------------------------------------------
// Named-parameter native calls
//
// UObject::Call only has a safe fast path for zero- and one-argument
// functions. With more arguments it walks the UFunction's property linked list
// and assigns the supplied arguments to it positionally - but that list is in
// reverse declaration order on the legacy builds Magnesium hosts, so the
// arguments land in the wrong parameter slots and the call silently does
// something else entirely. AFortSafeZoneIndicator::TrySetSafeZoneRadiusAndCenter
// exists for the same reason.
//
// Every multi-argument native call BotAI makes therefore packs its parameter
// buffer by reflected parameter NAME, validating each slot's size and bounds
// first, and fails closed when the layout is not what this build expects.
// ---------------------------------------------------------------------------

struct FNamedArgument
{
    const char* Name;
    const void* Data;
    uint32 Size;
};

bool CallByParameterName(
    const UObject* Object,
    UFunction* Function,
    std::initializer_list<FNamedArgument> Arguments,
    void* OutReturnValue = nullptr,
    uint32 ReturnValueSize = 0)
{
    if (!VersionFeatureAdapter::IsLiveObject(Object) ||
        !VersionFeatureAdapter::IsLiveObject(Function))
    {
        return false;
    }

    const auto Parameters = Function->GetParamsNamed();

    if (Parameters.Size == 0 || Parameters.Size > 0x400)
        return false;

    constexpr uint64 CPF_Parm = 0x0000000000000080;
    constexpr uint64 CPF_ReturnParm = 0x0000000000000400;

    // Resolve every requested argument to a validated offset before writing
    // anything, so a partially recognized layout writes nothing at all.
    uint32 Offsets[8]{};
    size_t Resolved = 0;

    if (Arguments.size() > std::size(Offsets))
        return false;

    for (const auto& Argument : Arguments)
    {
        bool bFound = false;

        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) ||
                (Parameter.PropertyFlags & CPF_ReturnParm) ||
                Parameter.Name != Argument.Name)
            {
                continue;
            }

            if (Parameter.ElementSize != Argument.Size ||
                Parameter.Offset > Parameters.Size ||
                Argument.Size > Parameters.Size - Parameter.Offset)
            {
                return false;
            }

            Offsets[Resolved] = Parameter.Offset;
            bFound = true;
            break;
        }

        if (!bFound)
            return false;

        ++Resolved;
    }

    auto Memory = FMemory::Malloc(Parameters.Size);

    if (!Memory)
        return false;

    memset((PBYTE)Memory, 0, Parameters.Size);

    size_t Index = 0;

    for (const auto& Argument : Arguments)
    {
        memcpy((PBYTE)Memory + Offsets[Index], Argument.Data, Argument.Size);
        ++Index;
    }

    // The return slot is resolved the same way: by flag, never by position.
    uint32 ReturnOffset = (uint32)-1;

    if (OutReturnValue && ReturnValueSize > 0)
    {
        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_ReturnParm) ||
                Parameter.ElementSize != ReturnValueSize ||
                Parameter.Offset > Parameters.Size ||
                ReturnValueSize > Parameters.Size - Parameter.Offset)
            {
                continue;
            }

            ReturnOffset = Parameter.Offset;
            break;
        }

        if (ReturnOffset == (uint32)-1)
        {
            FMemory::Free(Memory);
            return false;
        }
    }

    Object->ProcessEvent(Function, Memory);

    if (ReturnOffset != (uint32)-1)
        memcpy(OutReturnValue, (PBYTE)Memory + ReturnOffset, ReturnValueSize);

    FMemory::Free(Memory);
    return true;
}

// APawn::AddMovementInput(WorldDirection, ScaleValue, bForce).
bool AddMovementInput(
    AFortPlayerPawnAthena* Pawn, const FVector& Direction, float Scale)
{
    if (!VersionFeatureAdapter::IsLiveActor(Pawn))
        return false;

    static UFunction* Function = nullptr;

    if (!VersionFeatureAdapter::IsLiveObject(Function))
        Function = Pawn->GetFunction("AddMovementInput");

    const bool bForce = true;

    return CallByParameterName(
        Pawn, Function,
        {
            { "WorldDirection", &Direction, (uint32)FVector::Size() },
            { "ScaleValue", &Scale, (uint32)sizeof(Scale) },
            { "bForce", &bForce, (uint32)sizeof(bForce) },
        });
}

// ---------------------------------------------------------------------------
// Local control acquisition
// ---------------------------------------------------------------------------

// Fortnite's own ULocalPlayer subclass, falling back to the engine class.
// Cold lookup, retried on a miss: the package may not be loaded yet.
const UClass* GetLocalPlayerClass()
{
    static const UClass* Cached = nullptr;

    if (!Cached)
        Cached = FindClass("FortLocalPlayer");

    if (!Cached)
        Cached = FindClass("LocalPlayer");

    return Cached;
}

// Last resort when SpawnObject will not produce one: the host's own
// ULocalPlayer still exists on this build, it was only unregistered from
// UGameInstance::LocalPlayers to turn the session into a server. Its
// PlayerController field is left exactly as found.
UPlayer* FindExistingLocalPlayer(const UClass* LocalPlayerClass)
{
    if (!LocalPlayerClass)
        return nullptr;

    const int32 SkipFlags = Offsets::bEncryptedObjects ? 0x10200000 : 0x20;

    for (int Index = 0; Index < TUObjectArray::Num(); ++Index)
    {
        auto Item = TUObjectArray::GetItemByIndex(Index);

        if (!Item || (Item->GetFlags() & SkipFlags))
            continue;

        auto Object = Item->GetObject();

        if (!Object || !Object->Class || !Object->IsA(LocalPlayerClass))
            continue;

        // Class default objects are templates, not usable players.
        if (Object->Name.ToString().rfind("Default__", 0) == 0)
            continue;

        return (UPlayer*)Object;
    }

    return nullptr;
}

// Gives the bot controller a UPlayer of its own so the engine stops treating
// its pawn as somebody else's to simulate. The player object is deliberately
// NOT added to UGameInstance::LocalPlayers: nothing should iterate it, split
// screen should not see it, and it must never become a view target. It exists
// only so APlayerController::IsLocalController has something to say yes to.
//
// Every gate reports itself once per bot. A silent failure here looks exactly
// like a bot that will not move, so it is never allowed to be silent.
bool AcquireLocalControl(FBotEntity& Bot)
{
    if (!VersionFeatureAdapter::IsLiveActor(Bot.PC) ||
        !VersionFeatureAdapter::IsLiveActor(Bot.Pawn))
    {
        return false;
    }

    if (Bot.Pawn->IsLocallyControlled())
        return true;

    const bool bReport = Bot.LocalControlAttempts == 0;

    if (!Bot.PC->HasPlayer())
    {
        if (bReport)
        {
            SDK::DbgLog(
                "[BotAI] local control: controller %p has no reflected "
                "Player property on this build\n", (void*)Bot.PC);
        }

        return false;
    }

    // Something already owns this controller (a real connection, or a player
    // object from a previous attempt). Never replace it.
    if (VersionFeatureAdapter::IsLiveObject(Bot.PC->Player))
    {
        if (bReport)
        {
            SDK::DbgLog(
                "[BotAI] local control: controller %p already owns player %p "
                "but is still not locally controlled\n",
                (void*)Bot.PC, (void*)Bot.PC->Player);
        }

        return false;
    }

    auto LocalPlayerClass = GetLocalPlayerClass();

    if (!LocalPlayerClass)
    {
        if (bReport)
        {
            SDK::DbgLog(
                "[BotAI] local control: neither FortLocalPlayer nor "
                "LocalPlayer resolved on this build\n");
        }

        return false;
    }

    auto World = UWorld::GetWorld();
    auto Outer = World && World->HasOwningGameInstance()
        ? World->OwningGameInstance
        : nullptr;

    if (!VersionFeatureAdapter::IsLiveObject(Outer))
    {
        if (bReport)
        {
            SDK::DbgLog(
                "[BotAI] local control: no usable game instance to own the "
                "player object\n");
        }

        return false;
    }

    // UGameplayStatics::SpawnObject takes two arguments, so it has the same
    // positional-mapping hazard as everything else here: pack it by name.
    static UFunction* SpawnObjectFunction = nullptr;

    if (!VersionFeatureAdapter::IsLiveObject(SpawnObjectFunction))
    {
        SpawnObjectFunction =
            UGameplayStatics::GetDefaultObj()->GetFunction("SpawnObject");
    }

    const UClass* ObjectClass = LocalPlayerClass;
    UObject* SpawnedPlayer = nullptr;
    const bool bSpawnCallSucceeded =
        CallByParameterName(
            UGameplayStatics::GetDefaultObj(), SpawnObjectFunction,
            {
                { "ObjectClass", &ObjectClass, (uint32)sizeof(ObjectClass) },
                { "Outer", &Outer, (uint32)sizeof(Outer) },
            },
            &SpawnedPlayer, (uint32)sizeof(SpawnedPlayer));

    auto Player = VersionFeatureAdapter::IsLiveObject(SpawnedPlayer)
        ? (UPlayer*)SpawnedPlayer
        : nullptr;
    bool bReusedExisting = false;

    if (!Player)
    {
        Player = FindExistingLocalPlayer(LocalPlayerClass);
        bReusedExisting = Player != nullptr;
    }

    if (!Player)
    {
        if (bReport)
        {
            SDK::DbgLog(
                "[BotAI] local control: could not obtain a %s "
                "(spawnFn=%p spawnCall=%d spawned=%p, no existing instance)\n",
                LocalPlayerClass->GetName().ToString().c_str(),
                (void*)SpawnObjectFunction,
                bSpawnCallSucceeded ? 1 : 0, (void*)SpawnedPlayer);
        }

        return false;
    }

    // Only claim a freshly created player object. A reused one belongs to the
    // host and its controller link is left exactly as found.
    if (!bReusedExisting && Player->HasPlayerController())
        Player->PlayerController = Bot.PC;

    Bot.PC->Player = Player;

    const bool bLocal = Bot.Pawn->IsLocallyControlled();

    SDK::DbgLog(
        "[BotAI] local control: controller %p given %s %p (%s) -> "
        "locallyControlled=%d\n",
        (void*)Bot.PC, LocalPlayerClass->GetName().ToString().c_str(), (void*)Player,
        bReusedExisting ? "reused" : "new", bLocal ? 1 : 0);

    if (!bLocal)
    {
        // The engine still says no, so leave nothing attached behind.
        Bot.PC->Player = nullptr;
        return false;
    }

    return true;
}

// Hands the controller back to the engine exactly as it was found.
void ReleaseLocalControl(FBotEntity& Bot)
{
    if (!VersionFeatureAdapter::IsLiveActor(Bot.PC) || !Bot.PC->HasPlayer())
        return;

    auto Player = Bot.PC->Player;

    if (!VersionFeatureAdapter::IsLiveObject(Player))
        return;

    if (Player->HasPlayerController() && Player->PlayerController != Bot.PC)
        return; // not ours

    Bot.PC->Player = nullptr;
}

// Decides once per bot how its pawn can actually be moved, and says so.
void ResolveDriveMode(FBotEntity& Bot)
{
    if (!BotAISettings::bNativeMovement.load(std::memory_order_relaxed))
    {
        // Turning this off mid-match is authoritative: give the controller
        // back and re-decide, rather than keeping a stale native mode.
        if (Bot.DriveMode != EDriveMode::Swept)
        {
            ReleaseLocalControl(Bot);
            Bot.DriveMode = EDriveMode::Swept;
            Bot.LocalControlAttempts = 0;
        }

        return;
    }

    if (Bot.DriveMode != EDriveMode::Undecided)
        return;

    const bool bNative = AcquireLocalControl(Bot);

    // Possession and component setup can settle a frame or two after the
    // spawn command returns, so a first "no" is not final.
    if (!bNative && ++Bot.LocalControlAttempts < 5)
        return;

    Bot.DriveMode = bNative ? EDriveMode::Native : EDriveMode::Swept;

    SDK::DbgLog(
        "[BotAI] controller %p driving %s\n",
        (void*)Bot.PC, bNative ? "natively" : "swept");
}

// ---------------------------------------------------------------------------
// Movement diagnostics
//
// Answers one question from a live server instead of from a reading of the
// engine: does native character movement run for a connectionless spawnbot
// pawn? While enabled the swept driver is switched off and bots are steered
// with AddMovementInput alone, so anything that moves - or fails to - is the
// native path and nothing else.
//
// Read "accel" and "moved": non-zero means native input is being consumed and
// BotAI should be built on it. Both zero with localCtrl=0 means the movement
// component never ticks a move for these pawns.
// ---------------------------------------------------------------------------

// Reads a reflected property, reporting whether the build actually has it.
template <typename T>
bool TryReadProperty(const UObject* Object, const char* Name, T& Out)
{
    if (!VersionFeatureAdapter::IsLiveObject(Object))
        return false;

    const uint32 Offset = Object->GetOffset(Name);

    if (Offset == (uint32)-1 || Offset >= 0x10000)
        return false;

    Out = GetFromOffset<T>(Object, Offset);
    return true;
}

// Calls a reflected no-argument query. Returns false when the build has no
// such function, so a missing probe is never reported as a "no" answer.
bool TryCallBoolQuery(const UObject* Object, const char* Name, bool& Out)
{
    if (!VersionFeatureAdapter::IsLiveObject(Object))
        return false;

    auto Function = Object->GetFunction(Name);

    if (!VersionFeatureAdapter::IsLiveObject(Function))
        return false;

    Out = Object->Call<bool>(Function);
    return true;
}

void LogMovementDiagnostics(
    FBotEntity& Bot, const FVector& Direction, bool bInputPacked, float Now)
{
    const FVector Location = Bot.Pawn->K2_GetActorLocation();
    const double Moved = Distance2D(Location, Bot.DiagnosticLocation);
    Bot.DiagnosticLocation = Copy(Location);

    bool bLocallyControlled = false;
    const bool bHasLocallyControlled =
        TryCallBoolQuery(Bot.Pawn, "IsLocallyControlled", bLocallyControlled);

    bool bLocalController = false;
    const bool bHasLocalController =
        TryCallBoolQuery(Bot.PC, "IsLocalController", bLocalController);

    auto Movement =
        Bot.Pawn->HasCharacterMovement() &&
            VersionFeatureAdapter::IsLiveObject(Bot.Pawn->CharacterMovement)
        ? Bot.Pawn->CharacterMovement
        : nullptr;

    bool bOnGround = false;
    bool bSwimming = false;
    bool bFalling = false;
    uint8 MovementMode = 0xFF;
    FVector Acceleration{};
    float MaxWalkSpeed = -1.f;
    FVector Velocity{};

    if (Movement)
    {
        TryCallBoolQuery(Movement, "IsMovingOnGround", bOnGround);
        TryCallBoolQuery(Movement, "IsSwimming", bSwimming);
        TryCallBoolQuery(Movement, "IsFalling", bFalling);
        TryReadProperty(Movement, "MovementMode", MovementMode);
        TryReadProperty(Movement, "Acceleration", Acceleration);
        TryReadProperty(Movement, "MaxWalkSpeed", MaxWalkSpeed);
        Velocity = Movement->Velocity;
    }

    SDK::DbgLog(
        "[BotAI][Diag] pc=%p pawn=%p cmc=%p | localCtrl=%d(have=%d) "
        "locallyControlled=%d(have=%d) | pawnRole=%d/%d pcRole=%d/%d | "
        "moveMode=%u ground=%d swim=%d fall=%d | accel=%.1f vel=%.1f "
        "maxWalk=%.1f | inputPacked=%d dir=(%.2f,%.2f) moved=%.1f "
        "| drive=%d player=%p\n",
        (void*)Bot.PC, (void*)Bot.Pawn, (void*)Movement,
        bLocalController ? 1 : 0, bHasLocalController ? 1 : 0,
        bLocallyControlled ? 1 : 0, bHasLocallyControlled ? 1 : 0,
        Bot.Pawn->HasRole() ? (int)Bot.Pawn->Role : -1,
        Bot.Pawn->HasRemoteRole() ? (int)Bot.Pawn->RemoteRole : -1,
        Bot.PC->HasRole() ? (int)Bot.PC->Role : -1,
        Bot.PC->HasRemoteRole() ? (int)Bot.PC->RemoteRole : -1,
        (unsigned)MovementMode, bOnGround ? 1 : 0, bSwimming ? 1 : 0,
        bFalling ? 1 : 0,
        std::sqrt(
            Acceleration.X * Acceleration.X +
            Acceleration.Y * Acceleration.Y),
        std::sqrt(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y),
        MaxWalkSpeed, bInputPacked ? 1 : 0,
        Direction.X, Direction.Y, Moved,
        (int)Bot.DriveMode,
        Bot.PC->HasPlayer() ? (void*)Bot.PC->Player : nullptr);
}

// AActor::K2_SetActorRotation(NewRotation, bTeleportPhysics).
bool SetActorRotation(
    AFortPlayerPawnAthena* Pawn, const FRotator& Rotation)
{
    if (!VersionFeatureAdapter::IsLiveActor(Pawn))
        return false;

    static UFunction* Function = nullptr;

    if (!VersionFeatureAdapter::IsLiveObject(Function))
        Function = Pawn->GetFunction("K2_SetActorRotation");

    const bool bTeleportPhysics = false;

    return CallByParameterName(
        Pawn, Function,
        {
            { "NewRotation", &Rotation, (uint32)FRotator::Size() },
            { "bTeleportPhysics", &bTeleportPhysics,
              (uint32)sizeof(bTeleportPhysics) },
        });
}

// AActor::K2_TeleportTo(DestLocation, DestRotation).
bool TeleportTo(
    AFortPlayerPawnAthena* Pawn,
    const FVector& Location,
    const FRotator& Rotation)
{
    if (!VersionFeatureAdapter::IsLiveActor(Pawn))
        return false;

    static UFunction* Function = nullptr;

    if (!VersionFeatureAdapter::IsLiveObject(Function))
        Function = Pawn->GetFunction("K2_TeleportTo");

    return CallByParameterName(
        Pawn, Function,
        {
            { "DestLocation", &Location, (uint32)FVector::Size() },
            { "DestRotation", &Rotation, (uint32)FRotator::Size() },
        });
}

AFortPlayerPawnAthena* ResolvePawn(AFortPlayerControllerAthena* PC)
{
    if (!VersionFeatureAdapter::IsLiveActor(PC))
        return nullptr;

    AFortPlayerPawnAthena* Pawn = nullptr;

    if (PC->HasMyFortPawn() && VersionFeatureAdapter::IsLiveActor(PC->MyFortPawn))
        Pawn = PC->MyFortPawn;
    else if (VersionFeatureAdapter::IsLiveActor(PC->Pawn))
        Pawn = (AFortPlayerPawnAthena*)PC->Pawn;

    return Pawn;
}

// A pawn BotAI is allowed to steer this frame. Downed, dying and emoting
// pawns are left to their own native flows.
bool IsDrivablePawn(AFortPlayerPawnAthena* Pawn)
{
    if (!VersionFeatureAdapter::IsLiveActor(Pawn))
        return false;

    if (Pawn->HasbIsDying() && Pawn->bIsDying)
        return false;

    if (Pawn->HasbIsDBNO() && Pawn->bIsDBNO)
        return false;

    if (Pawn->HasbIsPlayingEmote() && Pawn->bIsPlayingEmote)
        return false;

    if (Pawn->GetHealth() <= 0.f)
        return false;

    return true;
}

// Presses jump and schedules the matching release.
void PressJump(FBotEntity& Bot, float Now)
{
    if (!Bot.Pawn || Bot.bJumpHeld)
        return;

    Bot.Pawn->Jump();
    Bot.bJumpHeld = true;
    Bot.ReleaseJumpTime = Now + 0.15f;
}

void ReleaseJumpIfDue(FBotEntity& Bot, float Now)
{
    if (!Bot.bJumpHeld || Now < Bot.ReleaseJumpTime)
        return;

    Bot.bJumpHeld = false;

    if (Bot.Pawn)
        Bot.Pawn->StopJumping();
}

bool IsSkydiving(AFortPlayerPawnAthena* Pawn)
{
    return Pawn && Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving;
}

void FaceDirection(
    FBotEntity& Bot,
    const FVector& Direction,
    float DeltaSeconds)
{
    if (!Bot.Pawn)
        return;

    const double Flat = std::sqrt(
        Direction.X * Direction.X + Direction.Y * Direction.Y);

    if (Flat < 0.001)
        return;

    FRotator Rotation = Bot.Pawn->K2_GetActorRotation();
    const float DesiredYaw =
        (float)(std::atan2((double)Direction.Y, (double)Direction.X) *
            57.2957795131);
    const float DeltaYaw = NormalizeYaw(DesiredYaw - (float)Rotation.Yaw);

    // Bound the turn rate so a re-picked goal behind the bot does not snap
    // both the actor and the replicated view yaw in a single frame.
    const float SafeDelta = (std::max)(0.f, (std::min)(DeltaSeconds, 0.05f));
    const float MaxTurn = 540.f * SafeDelta;

    Rotation.Pitch = 0.f;
    Rotation.Roll = 0.f;
    Rotation.Yaw += (std::max)(-MaxTurn, (std::min)(DeltaYaw, MaxTurn));

    SetActorRotation(Bot.Pawn, Rotation);

    if (VersionFeatureAdapter::IsLiveActor(Bot.PC))
        Bot.PC->SetControlRotation(Rotation);
}

// ---------------------------------------------------------------------------
// Native sprint
// ---------------------------------------------------------------------------

const UClass* GetSprintAbilityClass()
{
    // Cold lookup: the ability package may not be loaded when the first bot is
    // adopted, so a miss is retried rather than negatively cached.
    static const UClass* SprintClass = nullptr;

    if (!SprintClass)
        SprintClass = FindClass("FortGameplayAbility_Sprint");

    return SprintClass;
}

UAbilitySystemComponent* ResolveAbilitySystem(AFortPlayerControllerAthena* PC)
{
    if (!VersionFeatureAdapter::IsLiveActor(PC) || !PC->HasPlayerState())
        return nullptr;

    auto PlayerState = VersionFeatureAdapter::IsLiveActor(PC->PlayerState)
        ? PC->PlayerState->Cast<AFortPlayerStateAthena>()
        : nullptr;

    if (!PlayerState || !PlayerState->HasAbilitySystemComponent())
        return nullptr;

    auto AbilitySystem = PlayerState->AbilitySystemComponent;

    return VersionFeatureAdapter::IsLiveObject(AbilitySystem)
        ? AbilitySystem
        : nullptr;
}

// Holds or releases the native sprint input exactly the way a player's sprint
// key does, so the pawn picks up the game's own run speed and run animation
// instead of anything this module invents.
void SetSprinting(FBotEntity& Bot, bool bWantSprint, float Now)
{
    if (Bot.bSprintRequested == bWantSprint && !bWantSprint)
        return;

    auto AbilitySystem = ResolveAbilitySystem(Bot.PC);
    auto SprintClass = GetSprintAbilityClass();

    if (!AbilitySystem || !SprintClass)
        return;

    auto Spec = AbilitySystem->ActivatableAbilities.Items.Search(
        [SprintClass](FGameplayAbilitySpec& Item)
        {
            return Item.Ability &&
                VersionFeatureAdapter::IsLiveObject(Item.Ability) &&
                Item.Ability->IsA(SprintClass);
        },
        FGameplayAbilitySpec::Size());

    if (!Spec)
        return;

    Bot.bSprintRequested = bWantSprint;

    if (Spec->HasInputPressed() && Spec->InputPressed != bWantSprint)
    {
        Spec->InputPressed = bWantSprint;
        AbilitySystem->ActivatableAbilities.MarkItemDirty(*Spec);
    }

    if (!bWantSprint || (Bot.Pawn && Bot.Pawn->IsSprinting()))
        return;

    // The ability ends itself when the pawn stops or runs out of stamina, so
    // re-arm it on a cooldown rather than once per goal.
    if (Now < Bot.NextSprintAttemptTime)
        return;

    Bot.NextSprintAttemptTime = Now + 0.75f;

    const uint32 PredictionKeySize = FPredictionKey::Size();

    if (PredictionKeySize == 0 || PredictionKeySize > 0x100)
        return;

    auto PredictionKey = (FPredictionKey*)malloc(PredictionKeySize);

    if (!PredictionKey)
        return;

    memset((PBYTE)PredictionKey, 0, PredictionKeySize);
    UAbilitySystemComponent::InternalServerTryActivateAbility(
        AbilitySystem, Spec->Handle, true, PredictionKey, nullptr);
    free(PredictionKey);
}

// ---------------------------------------------------------------------------
// Safe zone
// ---------------------------------------------------------------------------

struct FZoneView
{
    bool bValid = false;
    FVector Center{};
    float Radius = 0.f;

    // True when the bot is far enough inside that it has no reason to rotate.
    bool bComfortable = true;
};

FZoneView EvaluateZone(const FVector& Location)
{
    FZoneView View;

    if (!BotAISettings::bSeekSafeZone.load(std::memory_order_relaxed))
        return View;

    if (!VersionFeatureAdapter::TryGetSafeZone(View.Center, View.Radius) ||
        View.Radius <= 0.f || !IsFinite(View.Center))
    {
        return View;
    }

    View.bValid = true;

    // Leave a margin proportional to the circle so bots start rotating while
    // the zone is still large instead of sprinting the last hundred metres.
    const float EdgeMargin =
        (std::max)(750.f, View.Radius * 0.15f);
    const double DistanceToCenter = Distance2D(Location, View.Center);

    View.bComfortable =
        DistanceToCenter <= (double)(View.Radius - EdgeMargin) &&
        VersionFeatureAdapter::IsInsideSafeZone(Location);

    return View;
}

// A random point on the ground, optionally constrained to a circle.
FVector PickGroundPoint(
    const FVector& Around,
    float MinDistance,
    float MaxDistance,
    const FZoneView& Zone,
    AFortPlayerPawnAthena* IgnorePawn)
{
    FVector Point = Around;

    for (int Attempt = 0; Attempt < 4; ++Attempt)
    {
        const float Angle = PlayerAIRandRange(0.f, 6.2831853f);
        const float Distance = PlayerAIRandRange(MinDistance, MaxDistance);

        Point = Copy(Around);
        Point.X += std::cos(Angle) * Distance;
        Point.Y += std::sin(Angle) * Distance;

        if (!Zone.bValid)
            break;

        // Keep wander goals well inside the circle so a bot never strolls
        // into the storm on its own.
        if (Distance2D(Point, Zone.Center) <= (double)(Zone.Radius * 0.75f))
            break;
    }

    bool bFoundGround = false;
    const FVector Ground =
        VersionFeatureAdapter::FindGroundLocation(
            Point, bFoundGround, IgnorePawn);

    return bFoundGround && IsFinite(Ground) ? Ground : Point;
}

// A point comfortably inside the circle, biased away from the exact centre so
// a whole lobby does not converge on one pixel.
FVector PickZoneEntryPoint(
    const FZoneView& Zone, AFortPlayerPawnAthena* IgnorePawn)
{
    const float Angle = PlayerAIRandRange(0.f, 6.2831853f);
    const float Radius =
        Zone.Radius * PlayerAIRandRange(0.15f, 0.55f);

    FVector Point = Zone.Center;
    Point.X += std::cos(Angle) * Radius;
    Point.Y += std::sin(Angle) * Radius;

    bool bFoundGround = false;
    const FVector Ground =
        VersionFeatureAdapter::FindGroundLocation(
            Point, bFoundGround, IgnorePawn);

    return bFoundGround && IsFinite(Ground) ? Ground : Point;
}

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------

// Releases the pawn's movement input. Native friction brings it to a stop,
// which reads correctly in the locomotion animation; writing the velocity
// directly would not.
void StopMoving(FBotEntity& Bot, float Now)
{
    SetSprinting(Bot, false, Now);
}

void SetGoal(FBotEntity& Bot, const FVector& Goal, float Now)
{
    Bot.Goal = Copy(Goal);
    Bot.bHasGoal = true;
    Bot.GoalSetTime = Now;
    Bot.ProgressLocation = Bot.Pawn
        ? Bot.Pawn->K2_GetActorLocation()
        : FVector{};
    Bot.LastProgressTime = Now;
}

void ClearGoal(FBotEntity& Bot, float Now)
{
    Bot.bHasGoal = false;
    StopMoving(Bot, Now);
}

// Publishes the movement the sweep actually achieved. Connected clients blend
// their walk/run locomotion from the replicated velocity of a pawn they do not
// simulate, so without this the bot glides in an idle pose.
void PublishAchievedVelocity(
    FBotEntity& Bot, const FVector& Before, const FVector& After,
    float DeltaSeconds)
{
    if (!Bot.Pawn || !Bot.Pawn->HasCharacterMovement() ||
        !VersionFeatureAdapter::IsLiveObject(Bot.Pawn->CharacterMovement) ||
        DeltaSeconds <= 0.f)
    {
        return;
    }

    FVector Velocity{};
    Velocity.X = (After.X - Before.X) / DeltaSeconds;
    Velocity.Y = (After.Y - Before.Y) / DeltaSeconds;
    Velocity.Z = (After.Z - Before.Z) / DeltaSeconds;

    Bot.Pawn->CharacterMovement->Velocity = Velocity;
}

// Sweeps the pawn one step toward Direction and applies swept gravity.
void StepSweptMovement(
    FBotEntity& Bot,
    const FVector& Direction,
    float Speed,
    float DeltaSeconds)
{
    const FVector Start = Bot.Pawn->K2_GetActorLocation();

    double MoveStep = (double)Speed * DeltaSeconds;

    if (MoveStep > 400.0)
        MoveStep = 400.0; // hitch guard: never leap on a long frame

    constexpr double StepUp = 55.0;    // stair/kerb allowance while grounded
    constexpr double StepDown = 140.0; // slope follow-down while grounded

    FVector Want = Start;
    Want.X = Start.X + Direction.X * MoveStep;
    Want.Y = Start.Y + Direction.Y * MoveStep;

    if (Bot.bGrounded)
        Want.Z = Start.Z + StepUp;

    Bot.Pawn->K2_SetActorLocation(Want, true, nullptr, false);

    const FVector Mid = Bot.Pawn->K2_GetActorLocation();

    Bot.VerticalVelocity -= 2200.f * DeltaSeconds;

    if (Bot.VerticalVelocity < -3800.f)
        Bot.VerticalVelocity = -3800.f;

    double VerticalDelta = (double)Bot.VerticalVelocity * DeltaSeconds;

    if (Bot.bGrounded)
        VerticalDelta -= StepUp + StepDown; // undo the lift, follow slopes down

    FVector VerticalTo = Mid;
    VerticalTo.Z = Mid.Z + VerticalDelta;

    Bot.Pawn->K2_SetActorLocation(VerticalTo, true, nullptr, false);

    const FVector End = Bot.Pawn->K2_GetActorLocation();

    if (VerticalDelta < 0.0)
    {
        // A blocked drop means the pawn is standing on real geometry; a
        // completed one means it is airborne and should keep falling.
        const bool bOnGround = End.Z > VerticalTo.Z + 1.0;

        if (bOnGround)
            Bot.VerticalVelocity = 0.f;

        Bot.bGrounded = bOnGround;
    }
    else if (End.Z < VerticalTo.Z - 1.0)
    {
        Bot.VerticalVelocity = 0.f; // bumped a ceiling: start falling
    }

    PublishAchievedVelocity(Bot, Start, End, DeltaSeconds);
}

// Returns true once the goal has been reached.
bool StepMovement(FBotEntity& Bot, float Now, float DeltaSeconds)
{
    if (!Bot.Pawn || !Bot.bHasGoal)
        return true;

    if (Now < Bot.PauseUntil)
    {
        StopMoving(Bot, Now);
        return false;
    }

    const FVector Location = Bot.Pawn->K2_GetActorLocation();
    const bool bDetouring = Now < Bot.DetourUntil;
    const FVector Target = bDetouring ? Bot.DetourGoal : Bot.Goal;

    FVector To = Target - Location;
    To.Z = 0.0;

    const double Distance = To.Magnitude();

    if (!bDetouring && Distance < GoalAcceptRadius)
    {
        ClearGoal(Bot, Now);
        return true;
    }

    if (Distance < 1.0)
        return false;

    const FVector Direction = To / Distance;

    // Face first: the body has to lead the sweep or the pawn moonwalks.
    FaceDirection(Bot, Direction, DeltaSeconds);

    // Diagnostics own the pawn while enabled: the swept driver is off, so the
    // only thing that can move it is native character movement, and the log
    // below is a clean reading of whether that happens.
    if (BotAISettings::bMovementDiagnostics.load(std::memory_order_relaxed))
    {
        const bool bInputPacked =
            AddMovementInput(Bot.Pawn, Direction, 1.f);

        if (Now >= Bot.NextDiagnosticTime)
        {
            Bot.NextDiagnosticTime = Now + 1.f;
            LogMovementDiagnostics(Bot, Direction, bInputPacked, Now);
        }

        return false;
    }

    // Long legs and zone rotations are worth running; short repositioning is
    // not.
    const bool bWantSprint =
        Bot.State == EBotAIState::SeekZone || Distance > SprintDistance;
    SetSprinting(Bot, bWantSprint, Now);

    if (Bot.DriveMode == EDriveMode::Native)
    {
        // Everything else - speed, acceleration, collision, gravity, stairs,
        // the walking/falling/swimming decision and the animation that goes
        // with it - belongs to the pawn's own movement component.
        AddMovementInput(Bot.Pawn, Direction, 1.f);
        return false;
    }

    StepSweptMovement(
        Bot, Direction, bWantSprint ? SprintSpeed : WalkSpeed, DeltaSeconds);
    return false;
}

// Horizontal steering while falling. Vertical motion stays entirely native.
void StepAirMovement(FBotEntity& Bot, float DeltaSeconds)
{
    if (!Bot.Pawn || !Bot.bHasLandingTarget)
        return;

    if (!Bot.Pawn->HasCharacterMovement() ||
        !VersionFeatureAdapter::IsLiveObject(Bot.Pawn->CharacterMovement))
    {
        return;
    }

    const FVector Location = Bot.Pawn->K2_GetActorLocation();

    FVector To = Bot.LandingTarget - Location;
    To.Z = 0.0;

    const double Distance = To.Magnitude();

    if (Distance < 1.0)
        return;

    const FVector Direction = To / Distance;

    FaceDirection(Bot, Direction, DeltaSeconds);

    if (Bot.DriveMode == EDriveMode::Native)
    {
        AddMovementInput(Bot.Pawn, Direction, 1.f);
        return;
    }

    // Without native movement the descent has to be steered directly. Taper
    // the glide as the bot arrives over the target so it does not sail past.
    const float Speed =
        (float)(std::min)(1400.0, (std::max)(0.0, Distance * 0.9));

    FVector Velocity = Bot.Pawn->CharacterMovement->Velocity;
    Velocity.X = Direction.X * Speed;
    Velocity.Y = Direction.Y * Speed;
    Bot.Pawn->CharacterMovement->Velocity = Velocity;
}

// ---------------------------------------------------------------------------
// Stuck recovery
// ---------------------------------------------------------------------------

void CheckStuck(FBotEntity& Bot, float Now)
{
    if (!Bot.bHasGoal || !Bot.Pawn || Now < Bot.PauseUntil)
        return;

    // A bot standing still is the expected result while diagnosing. Recovery
    // teleports would move it and make the reading meaningless.
    if (BotAISettings::bMovementDiagnostics.load(std::memory_order_relaxed))
        return;

    const FVector Location = Bot.Pawn->K2_GetActorLocation();

    if (Distance2D(Location, Bot.ProgressLocation) > 130.0)
    {
        Bot.ProgressLocation = Copy(Location);
        Bot.LastProgressTime = Now;
        Bot.StuckEscalation = 0;
        return;
    }

    if (Now - Bot.LastProgressTime < 2.f)
        return;

    Bot.ProgressLocation = Copy(Location);
    Bot.LastProgressTime = Now;
    ++Bot.StuckEscalation;

    switch (Bot.StuckEscalation)
    {
    case 1:
        // Most obstructions are a kerb, a fence or a doorway frame.
        PressJump(Bot, Now);
        break;

    case 2:
    {
        // Sidestep: aim 90 degrees off the blocked heading for a moment.
        FVector To = Bot.Goal - Location;
        To.Z = 0.0;

        const double Magnitude = To.Magnitude();

        if (Magnitude > 1.0)
        {
            const FVector Direction = To / Magnitude;
            const double Sign = PlayerAIRandChance(0.5f) ? 1.0 : -1.0;

            Bot.DetourGoal = Copy(Location);
            Bot.DetourGoal.X += -Direction.Y * Sign * 900.0;
            Bot.DetourGoal.Y += Direction.X * Sign * 900.0;
            Bot.DetourUntil = Now + 2.f;
        }

        PressJump(Bot, Now);
        break;
    }

    case 3:
        // The goal itself may simply be unreachable from here.
        ClearGoal(Bot, Now);
        Bot.NextThinkTime = 0.f;
        break;

    default:
    {
        // Native movement can be legitimately slow - swimming, wading, a steep
        // slope - and its ground truth is the riverbed, so never teleport a
        // natively driven bot. Re-planning is the only recovery it gets.
        if (Bot.DriveMode == EDriveMode::Native)
        {
            ClearGoal(Bot, Now);
            Bot.NextThinkTime = 0.f;
            Bot.StuckEscalation = 0;
            break;
        }

        // Last resort: a short traced hop toward the goal. Only ever moves the
        // bot onto real traced ground, never into geometry.
        FVector To = Bot.Goal - Location;
        To.Z = 0.0;

        const double Magnitude = To.Magnitude();

        if (Magnitude > 1.0)
        {
            const FVector Direction = To / Magnitude;
            FVector Desired = Location;
            Desired.X += Direction.X * (std::min)(Magnitude, 900.0);
            Desired.Y += Direction.Y * (std::min)(Magnitude, 900.0);

            FVector Spot{};

            if (VersionFeatureAdapter::TryResolveGroundedLandingSpot(
                    Desired, Bot.Pawn, Spot) &&
                IsFinite(Spot))
            {
                Spot.Z += 40.0;
                TeleportTo(
                    Bot.Pawn, Spot, Bot.Pawn->K2_GetActorRotation());
                Bot.Pawn->ForceNetUpdate();
            }
        }

        ClearGoal(Bot, Now);
        Bot.NextThinkTime = 0.f;
        Bot.StuckEscalation = 0;
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Per-bot think + tick
// ---------------------------------------------------------------------------

void Transition(FBotEntity& Bot, EBotAIState NewState, float Now)
{
    if (Bot.State == NewState)
        return;

    AIDebugLogger::Verbose(
        "BotAI", "%p: %s -> %s",
        (void*)Bot.PC, StateToString(Bot.State), StateToString(NewState));

    Bot.State = NewState;
    Bot.StateEnterTime = Now;
}

void Think(FBotEntity& Bot, float Now)
{
    const FVector Location = Bot.Pawn->K2_GetActorLocation();

    // ---- Transport ---------------------------------------------------------
    if (VersionFeatureAdapter::IsInAircraft(Bot.PC))
    {
        Transition(Bot, EBotAIState::InBus, Now);
        ClearGoal(Bot, Now);

        if (!Bot.bHasLandingTarget)
        {
            const FZoneView Zone = EvaluateZone(Location);

            Bot.LandingTarget = Zone.bValid
                ? PickZoneEntryPoint(Zone, Bot.Pawn)
                : PickGroundPoint(
                    Location, 0.f, 20000.f, Zone, Bot.Pawn);
            Bot.bHasLandingTarget = true;
        }

        if (Now >= Bot.NextJumpAttemptTime &&
            VersionFeatureAdapter::GetAircraftDropState(Now) ==
                EPlayerAIAircraftDropState::Open)
        {
            Bot.NextJumpAttemptTime = Now + 1.5f;

            if (VersionFeatureAdapter::JumpFromAircraft(Bot.PC))
            {
                Bot.Pawn = ResolvePawn(Bot.PC);

                if (Bot.Pawn)
                    VersionFeatureAdapter::TryBeginSkydiving(Bot.Pawn);

                Transition(Bot, EBotAIState::Falling, Now);
            }
        }

        return;
    }

    // ---- Falling -----------------------------------------------------------
    if (IsSkydiving(Bot.Pawn))
    {
        Transition(Bot, EBotAIState::Falling, Now);
        ClearGoal(Bot, Now);

        if (!Bot.bHasLandingTarget)
        {
            const FZoneView Zone = EvaluateZone(Location);

            Bot.LandingTarget = Zone.bValid
                ? PickZoneEntryPoint(Zone, Bot.Pawn)
                : Copy(Location);
            Bot.bHasLandingTarget = true;
        }

        return;
    }

    Bot.bHasLandingTarget = false;

    // ---- On foot -----------------------------------------------------------
    const FZoneView Zone = EvaluateZone(Location);

    // Survival first: a bot outside (or about to be outside) the circle drops
    // whatever it was strolling toward and rotates in.
    if (Zone.bValid && !Zone.bComfortable)
    {
        const bool bAlreadyRotating = Bot.State == EBotAIState::SeekZone;
        const bool bGoalStillInside =
            Bot.bHasGoal &&
            Distance2D(Bot.Goal, Zone.Center) <=
                (double)(Zone.Radius * 0.65f);

        Transition(Bot, EBotAIState::SeekZone, Now);
        Bot.PauseUntil = 0.f;

        if (!bAlreadyRotating || !bGoalStillInside || !Bot.bHasGoal)
            SetGoal(Bot, PickZoneEntryPoint(Zone, Bot.Pawn), Now);

        return;
    }

    Transition(Bot, EBotAIState::Wander, Now);

    const bool bGoalExpired =
        Bot.bHasGoal && Now - Bot.GoalSetTime > GoalTimeoutSeconds;

    if (!Bot.bHasGoal || bGoalExpired)
    {
        SetGoal(
            Bot,
            PickGroundPoint(
                Location, WanderMinDistance, WanderMaxDistance,
                Zone, Bot.Pawn),
            Now);

        // A short breather between hops reads as a player deciding where to
        // go next rather than a patrol route.
        if (BotAISettings::bIdleFlourishes.load(std::memory_order_relaxed) &&
            PlayerAIRandChance(0.25f))
        {
            Bot.PauseUntil = Now + PlayerAIRandRange(0.6f, 2.2f);
        }
    }
}

void TickBot(FBotEntity& Bot, float Now, float DeltaSeconds)
{
    ResolveDriveMode(Bot);
    ReleaseJumpIfDue(Bot, Now);

    if (Now >= Bot.NextThinkTime)
    {
        // Stagger so a full lobby spreads its planning across frames.
        Bot.NextThinkTime = Now + PlayerAIRandRange(0.25f, 0.45f);
        Think(Bot, Now);
    }

    if (Bot.State == EBotAIState::InBus)
        return;

    if (Bot.State == EBotAIState::Falling)
    {
        StepAirMovement(Bot, DeltaSeconds);
        return;
    }

    CheckStuck(Bot, Now);
    StepMovement(Bot, Now, DeltaSeconds);

    if (BotAISettings::bIdleFlourishes.load(std::memory_order_relaxed) &&
        Bot.bHasGoal && Now >= Bot.NextJumpTime)
    {
        Bot.NextJumpTime = Now + PlayerAIRandRange(9.f, 26.f);

        if (PlayerAIRandChance(0.35f))
            PressJump(Bot, Now);
    }
}

// ---------------------------------------------------------------------------
// Roster
// ---------------------------------------------------------------------------

// Adopts every spawnbot controller the command has registered, and forgets
// the ones that died, were removed with `delbot`, or belong to a stale world.
void RefreshRoster(float Now)
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    if (!GameMode)
    {
        GBots.clear();
        return;
    }

    for (auto Iterator = GBots.begin(); Iterator != GBots.end();)
    {
        auto& Bot = Iterator->second;

        if (!VersionFeatureAdapter::IsLiveActor(Bot.PC) ||
            !AFortPlayerControllerAthena::IsCheatSpawnedBotController(Bot.PC))
        {
            ReleaseLocalControl(Bot);
            Iterator = GBots.erase(Iterator);
            continue;
        }

        Bot.Pawn = ResolvePawn(Bot.PC);
        ++Iterator;
    }

    for (auto Controller : GameMode->AlivePlayers)
    {
        auto PC = (AFortPlayerControllerAthena*)Controller;

        if (!VersionFeatureAdapter::IsLiveActor(PC) ||
            !AFortPlayerControllerAthena::IsCheatSpawnedBotController(PC) ||
            GBots.contains(PC))
        {
            continue;
        }

        FBotEntity Bot;
        Bot.PC = PC;
        Bot.Pawn = ResolvePawn(PC);
        Bot.StateEnterTime = Now;
        Bot.LastProgressTime = Now;
        Bot.ProgressLocation = Bot.Pawn
            ? Bot.Pawn->K2_GetActorLocation()
            : FVector{};
        // Spread the very first think out so a `spawnbot 50` burst does not
        // plan fifty routes in the frame it lands.
        Bot.NextThinkTime = Now + PlayerAIRandRange(0.f, 1.f);
        Bot.NextJumpTime = Now + PlayerAIRandRange(6.f, 20.f);

        GBots.emplace(PC, Bot);

        AIDebugLogger::Verbose(
            "BotAI", "adopted spawnbot controller %p", (void*)PC);
    }
}

bool IsManagedThunk(const AFortPlayerControllerAthena* PC)
{
    return BotAI::IsManaged(PC);
}

bool HasManagedThunk()
{
    return BotAI::HasManaged();
}

} // namespace

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

bool BotAI::IsManaged(const AFortPlayerControllerAthena* PC)
{
    if (!PC || GBots.empty())
        return false;

    return GBots.contains(
        const_cast<AFortPlayerControllerAthena*>(PC));
}

bool BotAI::HasManaged()
{
    return !GBots.empty();
}

int BotAI::GetActiveCount()
{
    return (int)GBots.size();
}

const char* BotAI::GetStatusLine()
{
    return GStatusLine;
}

void BotAI::Shutdown(const char* Reason)
{
    if (GBots.empty())
        return;

    // Hand each bot back to the engine as it was found: no sprint held, and
    // no player object left attached to its controller.
    const float Now = VersionFeatureAdapter::GetTimeSeconds();

    for (auto& [PC, Bot] : GBots)
    {
        StopMoving(Bot, Now);
        ReleaseLocalControl(Bot);
    }

    SDK::DbgLog(
        "[BotAI] released %d bot(s): %s\n",
        (int)GBots.size(), Reason ? Reason : "");

    GBots.clear();
    snprintf(GStatusLine, sizeof(GStatusLine), "Bot AI: idle");
}

void BotAI::OnServerTick(UNetDriver* Driver, float DeltaSeconds)
{
    auto World = UWorld::GetWorld();

    if (!World || !Driver || Driver != World->NetDriver)
        return;

    if (!GHooksRegistered)
    {
        GHooksRegistered = true;
        VersionFeatureAdapter::SetManagedAIControllerHooks(
            &IsManagedThunk, &HasManagedThunk);
    }

    // A new world (map travel, next match) invalidates every tracked pointer.
    if (GWorldToken != World)
    {
        GWorldToken = World;
        GBots.clear();
        GNextDiscoveryTime = 0.f;
        VersionFeatureAdapter::ResetCaches();
        snprintf(GStatusLine, sizeof(GStatusLine), "Bot AI: idle");
    }

    const float Now = VersionFeatureAdapter::GetTimeSeconds();
    VersionFeatureAdapter::BeginServerTick(Now);

    // Cheat-command bots share the version-aware cosmetic cache whether or not
    // BotAI is driving anything, so this runs before the enable check.
    VersionFeatureAdapter::TickCosmeticCache();
    VersionFeatureAdapter::RetryPendingPlayersLeftReplication();

    if (!BotAISettings::bEnabled.load(std::memory_order_relaxed))
    {
        // Turning the toggle off is authoritative in every phase: already
        // adopted bots go dormant again instead of finishing their route.
        Shutdown("Bot AI turned off");
        return;
    }

    if (GUI::gsStatus.load(std::memory_order_acquire) < Joinable)
        return;

    if (Now >= GNextDiscoveryTime)
    {
        GNextDiscoveryTime = Now + 0.5f;
        RefreshRoster(Now);
    }

    if (GBots.empty())
    {
        snprintf(GStatusLine, sizeof(GStatusLine), "Bot AI: on, no bots");
        return;
    }

    const float SafeDelta =
        std::isfinite((double)DeltaSeconds)
            ? (std::max)(0.f, (std::min)(DeltaSeconds, 0.1f))
            : 0.f;

    int Wandering = 0;
    int Rotating = 0;

    for (auto Iterator = GBots.begin(); Iterator != GBots.end();)
    {
        auto& Bot = Iterator->second;

        if (!VersionFeatureAdapter::IsLiveActor(Bot.PC))
        {
            Iterator = GBots.erase(Iterator);
            continue;
        }

        if (!IsDrivablePawn(Bot.Pawn))
        {
            // Downed, dying or emoting: leave the pawn alone this frame and
            // re-plan from scratch when it becomes drivable again.
            Bot.Pawn = ResolvePawn(Bot.PC);
            Bot.bHasGoal = false;
            Bot.NextThinkTime = 0.f;
            ++Iterator;
            continue;
        }

        TickBot(Bot, Now, SafeDelta);

        if (Bot.State == EBotAIState::SeekZone)
            ++Rotating;
        else if (Bot.State == EBotAIState::Wander)
            ++Wandering;

        ++Iterator;
    }

    snprintf(
        GStatusLine, sizeof(GStatusLine),
        "Bot AI: driving %d (%d wandering, %d rotating)",
        (int)GBots.size(), Wandering, Rotating);
}
