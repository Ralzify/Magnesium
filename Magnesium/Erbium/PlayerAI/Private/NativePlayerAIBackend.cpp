#include "pch.h"
// ============================================================================
// Magnesium PlayerAI - NativePlayerAIBackend implementation
//
// Uses the engine's own player-bot (Phoebe) stack where it exists:
//   - FortAthenaMutator_Bots::SpawnBot for spawning (native registration,
//     replication, alive/bot lists, unique ids, minimap counting)
//   - runtime navmesh generation + AAIController::MoveToLocation pathing
//   - native sprint / jump / crouch / weapon fire (real bullets, real
//     damage, real kill credit through the existing pipeline)
// ============================================================================
#include "../Public/NativePlayerAIBackend.h"
#include "../Public/PlayerAIController.h"
#include "../Public/PlayerAIConfig.h"
#include "../Public/PlayerAIFaultGuard.h"
#include "../Public/AIDebugLogger.h"
#include "../Public/VersionFeatureAdapter.h"
#include "../Public/ReplicationBehavior.h"
#include "../../../FortniteGame/Public/FortKismetLibrary.h"
#include "../../../FortniteGame/Public/FortWeaponMods.h"
#include "../../../Engine/Public/AbilitySystemComponent.h"

// ============================================================================
// PlayerAIEntity::GetPawn (declared in PlayerAIEntity.h)
// ============================================================================

static bool PlayerAIIsLiveObject(const UObject* Object)
{
    if (!Object || !SDK::MemReadable(Object, sizeof(UObject)))
        return false;

    const int32 ObjectIndex = Object->Index;

    if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
        return false;

    auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
    const int32 InvalidObjectFlags =
        Offsets::bEncryptedObjects ? 0x10200000 : 0x20;

    return Item && Item->GetObject() == Object &&
        !(Item->GetFlags() & InvalidObjectFlags) &&
        Object->Class && SDK::MemReadable(Object->Class, sizeof(UClass));
}

static bool PlayerAIIsLiveActor(const AActor* Actor)
{
    if (!PlayerAIIsLiveObject(Actor))
        return false;

    return !Actor->HasbActorIsBeingDestroyed() ||
        !Actor->bActorIsBeingDestroyed;
}

static bool PlayerAIIsLiveActorOfClass(
    const AActor* Actor, const UClass* ExpectedClass)
{
    return ExpectedClass && PlayerAIIsLiveActor(Actor) &&
        Actor->IsA(ExpectedClass);
}

bool PlayerAIEntity::HasLiveController() const
{
    const AActor* Controller = bNativeBacked
        ? NativeController : (const AActor*)PC;
    const UClass* ControllerClass = bNativeBacked
        ? AFortAthenaAIBotController::StaticClass()
        : AFortPlayerControllerAthena::StaticClass();

    return PlayerAIIsLiveActorOfClass(Controller, ControllerClass);
}

bool PlayerAIEntity::HasLivePlayerState() const
{
    return PlayerAIIsLiveActorOfClass(
        PlayerState, AFortPlayerStateAthena::StaticClass());
}

bool PlayerAIEntity::IsValid() const
{
    return HasLiveController() && HasLivePlayerState();
}

bool PlayerAIEntity::IsLivePawn(
    const AFortPlayerPawnAthena* Pawn)
{
    return PlayerAIIsLiveActorOfClass(
        Pawn, AFortPlayerPawnAthena::StaticClass());
}

AFortPlayerPawnAthena* PlayerAIEntity::GetPawn() const
{
    const UClass* PawnClass =
        AFortPlayerPawnAthena::StaticClass();

    if (bNativeBacked)
    {
        auto Bot = (AFortAthenaAIBotController*)NativeController;

        if (HasLiveController())
        {
            auto Pawn = Bot->Pawn;

            if (PlayerAIIsLiveActorOfClass(Pawn, PawnClass))
                return Pawn;
        }

        return PlayerAIIsLiveActorOfClass(
            NativePawn, PawnClass) ? NativePawn : nullptr;
    }

    if (!HasLiveController())
        return nullptr;

    // Pawn is the possession authority. MyFortPawn can retain the address of
    // a destroyed warmup pawn after aircraft transitions on several builds.
    auto Pawn = PC->Pawn;

    if (!PlayerAIIsLiveActorOfClass(Pawn, PawnClass))
        Pawn = PC->MyFortPawn;

    return PlayerAIIsLiveActorOfClass(
        Pawn, PawnClass) ? (AFortPlayerPawnAthena*)Pawn : nullptr;
}

AFortPlayerPawnAthena* PlayerAIEntity::GetNativeControllerPawn() const
{
    if (!bNativeBacked || !HasLiveController())
        return nullptr;

    auto Bot = (AFortAthenaAIBotController*)NativeController;
    auto Pawn = Bot->Pawn;

    return PlayerAIIsLiveActorOfClass(
        Pawn, AFortPlayerPawnAthena::StaticClass())
        ? Pawn : nullptr;
}

AFortInventory* PlayerAIEntity::GetInventory() const
{
    if (!HasLiveController())
        return nullptr;

    AFortInventory* Inventory = nullptr;

    if (bNativeBacked)
    {
        auto Bot =
            (AFortAthenaAIBotController*)NativeController;

        if (Bot->HasInventory())
            Inventory = Bot->Inventory;
    }
    else
    {
        Inventory = PC->WorldInventory;
    }

    return PlayerAIIsLiveActorOfClass(
        Inventory, AFortInventory::StaticClass())
        ? Inventory : nullptr;
}

// ============================================================================
// Detection
// ============================================================================

static int GNativeAvailable = -1;
static AActor* GBotMutator = nullptr;
static bool bNavMeshRequested = false;
static bool bNavMeshReady = false;
static const UClass* GPhoebePawnClass = nullptr;
static const UClass* GMutatorClass = nullptr;
static int GDetectAttempts = 0;
static float GNextDetectTime = 0.f;
static int GConsecutiveNullSpawns = 0;

// Resident-only class lookup. Native Phoebe support is optional, so its
// detection must never force package loading from TickFlush.
// (Kept free of unwindable C++ objects for SEH.)
static const UClass* PlayerAITryFindLoadedClass(
    const wchar_t* Path, bool* bOutFaulted)
{
    if (!Offsets::StaticFindObject)
        return nullptr;

    GPlayerAIGuardedNativeCallDepth++;

    const UClass* Result = nullptr;
    bool bFaulted = false;

    __try
    {
        Result = (const UClass*)SDK::StaticFindObject(
            Path, UClass::StaticClass());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bFaulted = true;
    }

    GPlayerAIGuardedNativeCallDepth--;

    if (bOutFaulted)
        *bOutFaulted = bFaulted;

    return Result;
}

// Detection uses only classes already present in the hosted build's active
// object set. If Phoebe is not resident, the universal simulated backend is
// safer than synchronously loading its package from the server tick.
static void PlayerAIDetectNativeStack()
{
    if (GNativeAvailable != -1)
        return;

    // The player-bot (Phoebe) stack shipped with Chapter 2 (11.00). On older
    // versions there is nothing to load - never touch the loader there.
    if (VersionInfo.FortniteVersion < 11.00)
    {
        GNativeAvailable = 0;
        AIDebugLogger::Log("NativeBackend", "pre-11.00 version - PlayerAI uses the simulated backend");
        return;
    }

    const float Now = VersionFeatureAdapter::GetTimeSeconds();

    if (Now < GNextDetectTime)
        return;

    GNextDetectTime = Now + 2.f;
    GDetectAttempts++;

    // Native classes are process-resident. Cache both success and failure so
    // four availability probes do not repeat full reflected-object scans.
    static auto BotControllerClass =
        FindClass("FortAthenaAIBotController");
    static auto NativeMutatorClass =
        FindClass("FortAthenaMutator_Bots");
    bool bLoadFaulted = false;

    if (!GPhoebePawnClass)
        GPhoebePawnClass = PlayerAITryFindLoadedClass(L"/Game/Athena/AI/Phoebe/BP_PlayerPawn_Athena_Phoebe.BP_PlayerPawn_Athena_Phoebe_C", &bLoadFaulted);

    if (!GMutatorClass && !bLoadFaulted)
        GMutatorClass = PlayerAITryFindLoadedClass(L"/Game/Athena/AI/Phoebe/BP_Phoebe_Mutator.BP_Phoebe_Mutator_C", &bLoadFaulted);

    if (!GMutatorClass)
        GMutatorClass = NativeMutatorClass;

    if (bLoadFaulted)
    {
        // Loading faulted on this build: settle immediately, never retry.
        GNativeAvailable = 0;
        AIDebugLogger::MissingFeature("NativeBotAssetLoading",
            "resident asset lookup faulted - PlayerAI uses the simulated backend");
        return;
    }

    if (BotControllerClass && GPhoebePawnClass && GMutatorClass)
    {
        GNativeAvailable = 1;
        AIDebugLogger::Log("NativeBackend", "resident native player-bot stack found - PlayerAI uses engine bots (navmesh pathing, real weapons)");
        return;
    }

    if (GDetectAttempts >= 4)
    {
        GNativeAvailable = 0;
        AIDebugLogger::Log("NativeBackend",
            "no native player-bot stack on this version (controller %d, pawn %d, mutator %d) - PlayerAI uses the simulated backend",
            BotControllerClass ? 1 : 0, GPhoebePawnClass ? 1 : 0, GMutatorClass ? 1 : 0);
    }
}

bool NativePlayerAIBackend::IsAvailable()
{
    PlayerAIDetectNativeStack();
    return GNativeAvailable == 1;
}

void NativePlayerAIBackend::Reset()
{
    // This actor is created by this backend, so tear it down before dropping
    // the pointer. Same-world match recycling would otherwise accumulate bot
    // mutators, while a destroyed actor could later be mistaken for a valid
    // initialized backend.
    if (PlayerAIIsLiveActor(GBotMutator))
        GBotMutator->K2_DestroyActor();

    GNativeAvailable = -1;
    GBotMutator = nullptr;
    bNavMeshRequested = false;
    bNavMeshReady = false;
    GPhoebePawnClass = nullptr;
    GMutatorClass = nullptr;
    GDetectAttempts = 0;
    GNextDetectTime = 0.f;
    GConsecutiveNullSpawns = 0;
}

bool NativePlayerAIBackend::IsNavMeshReady()
{
    return bNavMeshReady;
}

// ============================================================================
// Navmesh bootstrap
// ============================================================================

// Use nav data the hosted build already initialized. Streaming a bounds level,
// scaling it over the whole island, and synchronously notifying navigation
// launched a full dynamic rebuild from TickFlush; on several builds that
// starved the game thread indefinitely. MoveTo has a direct-input fallback
// whenever the existing path follower cannot find a route.
static void PlayerAIEnsureNavMesh()
{
    if (bNavMeshRequested)
        return;

    auto World = UWorld::GetWorld();

    if (!World || !World->HasNavigationSystem() || !World->NavigationSystem)
        return;

    bNavMeshRequested = true;

    auto NavSystem = World->NavigationSystem;
    static auto MainNavDataOffset = NavSystem->GetOffset("MainNavData");

    if (MainNavDataOffset != -1)
    {
        auto NavData = GetFromOffset<AActor*>(NavSystem, MainNavDataOffset);

        if (NavData)
        {
            bNavMeshReady = true;
            AIDebugLogger::Log(
                "NativeBackend",
                "using the hosted build's existing navigation data");
            return;
        }
    }

    AIDebugLogger::MissingFeature(
        "ExistingNavDataForPlayerAI",
        "no ready nav data - native bots use bounded direct-input movement");
}

// ============================================================================
// Initialization (mutator + bot manager wiring)
// ============================================================================

void NativePlayerAIBackend::EnsureInitialized()
{
    if (!IsAvailable())
        return;

    PlayerAIEnsureNavMesh();

    if (PlayerAIIsLiveActorOfClass(
            GBotMutator, GMutatorClass))
        return;

    GBotMutator = nullptr;

    auto GameMode = VersionFeatureAdapter::GetGameMode();
    auto GameState = VersionFeatureAdapter::GetGameState();

    if (!GameMode || !GameState || !GMutatorClass)
        return;

    GBotMutator = UWorld::SpawnActor(GMutatorClass, FVector{}, FRotator{}, GameMode);

    if (!GBotMutator)
    {
        AIDebugLogger::Error("NativeBackend", "failed to spawn the player-bot mutator - falling back to the simulated backend");
        GNativeAvailable = 0;
        return;
    }

    // The mutator needs its cached game mode/state before SpawnBot works.
    {
        auto CachedGameModeOffset = GBotMutator->GetOffset("CachedGameMode");

        if (CachedGameModeOffset != -1)
            GetFromOffset<AActor*>(GBotMutator, CachedGameModeOffset) = (AActor*)GameMode;

        auto CachedGameStateOffset = GBotMutator->GetOffset("CachedGameState");

        if (CachedGameStateOffset != -1)
            GetFromOffset<AActor*>(GBotMutator, CachedGameStateOffset) = (AActor*)GameState;
    }

    // Wire the mutator into the server bot manager Magnesium already spawns.
    if (GameMode->HasServerBotManager() && GameMode->ServerBotManager)
    {
        auto BotManager = (UFortServerBotManagerAthena*)GameMode->ServerBotManager;

        if (BotManager->HasCachedBotMutator())
            BotManager->CachedBotMutator = (AActor*)GBotMutator;
    }

    AIDebugLogger::Log("NativeBackend", "player-bot mutator ready");
}

// ============================================================================
// Spawning
// ============================================================================

PlayerAIEntity NativePlayerAIBackend::SpawnNativeEntity(const FTransform& SpawnAt, const std::string& DisplayName)
{
    PlayerAIEntity Entity{};

    EnsureInitialized();

    if (!PlayerAIIsLiveActorOfClass(
            GBotMutator, GMutatorClass) ||
        !GPhoebePawnClass)
    {
        GBotMutator = nullptr;
        return Entity;
    }

    auto SpawnBotFn = GBotMutator->GetFunction("SpawnBot");

    if (!SpawnBotFn)
    {
        AIDebugLogger::MissingFeature("FortAthenaMutator_Bots.SpawnBot", "native bot spawning unavailable - simulated backend takes over");
        GNativeAvailable = 0;
        return Entity;
    }

    FTransform Transform = const_cast<FTransform&>(SpawnAt);
    FVector Loc = Transform.Translation;
    FRotator Rot{};

    // Temporary spawn locator actor (the native spawn path wants one).
    static auto LocatorClass = FindClass("TargetPoint");
    AActor* Locator = LocatorClass ? UWorld::SpawnActor(LocatorClass, Loc, Rot) : nullptr;

    auto Pawn = GBotMutator->Call<AFortPlayerPawnAthena*>(SpawnBotFn, GPhoebePawnClass, Locator, Loc, Rot, false);

    if (!Pawn)
        Pawn = GBotMutator->Call<AFortPlayerPawnAthena*>(SpawnBotFn, GPhoebePawnClass, Locator, Loc, Rot, true);

    if (Locator)
        Locator->K2_DestroyActor();

    // Consecutive dead spawns disable the backend for the session so the
    // simulated tier takes over (a null-returning SpawnBot otherwise
    // retries forever and the lobby never fills - seen on 17.30).
    if (!Pawn)
    {
        AIDebugLogger::Error("NativeBackend", "native bot spawn returned null");

        if (++GConsecutiveNullSpawns >= 3)
        {
            GNativeAvailable = 0;
            AIDebugLogger::MissingFeature("NativeBotSpawn",
                "native bot spawn keeps returning null - simulated backend takes over");
        }

        return Entity;
    }

    auto Bot = (AFortAthenaAIBotController*)Pawn->Controller;

    if (!Bot)
    {
        AIDebugLogger::Error("NativeBackend", "native bot spawned without a controller");
        Pawn->K2_DestroyActor();

        if (++GConsecutiveNullSpawns >= 3)
        {
            GNativeAvailable = 0;
            AIDebugLogger::MissingFeature("NativeBotSpawn",
                "native bot spawns keep coming up broken - simulated backend takes over");
        }

        return Entity;
    }

    auto PlayerState = Bot->HasPlayerState() ? Bot->PlayerState : nullptr;

    if (!PlayerState)
    {
        AIDebugLogger::Error("NativeBackend", "native bot spawned without a player state");
        Pawn->K2_DestroyActor();

        if (++GConsecutiveNullSpawns >= 3)
        {
            GNativeAvailable = 0;
            AIDebugLogger::MissingFeature("NativeBotSpawn",
                "native bot spawns keep coming up broken - simulated backend takes over");
        }

        return Entity;
    }

    GConsecutiveNullSpawns = 0;

    // Inventory (native bots use the controller's own inventory).
    if (!Bot->HasInventory() || !Bot->Inventory)
    {
        auto Inv = (AFortInventory*)UWorld::SpawnActor(AFortInventory::StaticClass(), FVector(0, 0, -99999.f));

        if (Inv)
        {
            Inv->SetOwner(Bot);
            Inv->InventoryType = 0;

            if (Bot->HasInventory())
                Bot->Inventory = Inv;
        }
    }

    // Pickaxe.
    if (Bot->HasInventory() && Bot->Inventory)
    {
        static auto DefaultPickaxe =
            Offsets::StaticFindObject
            ? (const UFortItemDefinition*)SDK::StaticFindObject(
                L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01",
                UFortItemDefinition::StaticClass())
            : nullptr;

        if (DefaultPickaxe)
        {
            auto Item = Bot->Inventory->GiveItem(DefaultPickaxe);

            if (Item)
            {
                auto Weapon = (AFortWeapon*)Pawn->EquipWeaponDefinition(
                    (UFortItemDefinition*)DefaultPickaxe,
                    Item->ItemEntry.ItemGuid);
                if (Weapon)
                {
                    FFortWeaponMods::ApplyEntrySlotsAfterEquip(
                        Weapon, Item->ItemEntry);
                }
            }
        }
    }

    // Wire the path following component to the (runtime generated) navmesh.
    if (bNavMeshReady && Bot->HasPathFollowingComponent() && Bot->PathFollowingComponent)
    {
        auto World = UWorld::GetWorld();
        auto NavSystem = World && World->HasNavigationSystem() ? World->NavigationSystem : nullptr;

        if (NavSystem)
        {
            static auto MainNavDataOffset = NavSystem->GetOffset("MainNavData");

            if (MainNavDataOffset != -1)
            {
                auto NavData = GetFromOffset<AActor*>(NavSystem, MainNavDataOffset);
                auto PathComp = Bot->PathFollowingComponent;
                static auto MyNavDataOffset = PathComp->GetOffset("MyNavData");

                if (NavData && MyNavDataOffset != -1)
                {
                    GetFromOffset<AActor*>(PathComp, MyNavDataOffset) = NavData;

                    auto RegisteredFn = PathComp->GetFunction("OnNavDataRegistered");

                    if (RegisteredFn)
                        PathComp->Call<void>(RegisteredFn, NavData);
                }
            }
        }
    }

    // Unique team per PlayerAI in solo-style fills so they fight each other
    // (the native spawn can hand out colliding team indices).
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    if (GameMode && PlayerState->HasTeamIndex())
    {
        uint8 MaxTeam = 3;

        auto ScanList = [&](TArray<AActor*>& List)
            {
                for (auto& Uncasted : List)
                {
                    auto Controller = (AFortPlayerControllerAthena*)Uncasted;

                    if (!Controller)
                        continue;

                    auto PS = (AFortPlayerStateAthena*)Controller->PlayerState;

                    if (PS && PS->TeamIndex > MaxTeam)
                        MaxTeam = PS->TeamIndex;
                }
            };

        ScanList(GameMode->AlivePlayers);

        if (GameMode->HasAliveBots())
            ScanList(GameMode->AliveBots);

        if (PlayerState->TeamIndex <= MaxTeam)
        {
            PlayerState->TeamIndex = MaxTeam + 1;

            auto OnRepTeam = PlayerState->GetFunction("OnRep_TeamIndex");

            if (OnRepTeam)
            {
                uint8 OldTeam = PlayerState->TeamIndex;
                PlayerState->Call<void>(OnRepTeam, OldTeam);
            }

            if (PlayerState->HasSquadId())
            {
                PlayerState->SquadId = PlayerState->TeamIndex - 3;
                PlayerState->OnRep_SquadId();
            }
        }
    }

    Entity.NativeController = Bot;
    Entity.NativePawn = Pawn;
    Entity.PlayerState = PlayerState;
    Entity.DisplayName = DisplayName;
    Entity.bNativeBacked = true;
    return Entity;
}

// ============================================================================
// Movement / action primitives
// ============================================================================

AFortAthenaAIBotController* NativePlayerAIBackend::GetBotController(PlayerAIController& AI)
{
    if (!AI.Entity.bNativeBacked ||
        !AI.Entity.HasLiveController())
    {
        return nullptr;
    }

    return (AFortAthenaAIBotController*)
        AI.Entity.NativeController;
}

static uint8 PlayerAIMoveStatusMoving()
{
    static int Cached = -1;

    if (Cached == -1)
    {
        auto Enum = FindEnum("EPathFollowingStatus");
        Cached = Enum ? (int)Enum->GetValue("Moving") : -1;

        if (Cached < 0)
            Cached = 3; // engine default
    }

    return (uint8)Cached;
}

void NativePlayerAIBackend::Sprint(PlayerAIController& AI)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    // Do not activate sprint through InternalServerTryActivateAbility. Raw
    // ability activation on server-owned, connectionless players can re-enter
    // the native activation path until the game thread exhausts its stack.
    // Movement remains engine-owned; the replicated style supplies the sprint
    // presentation without that unsafe call.
    AI.bSprintAbilityActivated = true;

    if (Pawn->HasCurrentMovementStyle())
    {
        static int SprintingValue = -2;

        if (SprintingValue == -2)
        {
            auto Enum = FindEnum("EFortMovementStyle");
            SprintingValue = Enum ? (int)Enum->GetValue("Sprinting") : -1;
        }

        if (SprintingValue >= 0 && Pawn->CurrentMovementStyle != (uint8)SprintingValue)
            Pawn->CurrentMovementStyle = (uint8)SprintingValue;
    }
}

void NativePlayerAIBackend::MoveTo(PlayerAIController& AI, const FVector& Dest, float AcceptRadius, bool bSprint)
{
    auto Bot = GetBotController(AI);
    auto Pawn = AI.GetPawn();

    if (!Bot || !Pawn)
        return;

    const float Now = VersionFeatureAdapter::GetTimeSeconds();

    FVector Loc = Pawn->K2_GetActorLocation();
    FVector Flat = Dest - Loc;
    Flat.Z = 0;
    const double Dist = Flat.Magnitude();

    if (Dist <= AcceptRadius)
        return;

    const bool bGoalChanged =
        !AI.bHasIssuedMoveTarget ||
        (Dest - AI.LastIssuedMoveTarget).Magnitude() >
            500.0;

    AI.MoveTarget = FVector(Dest);
    AI.bHasMoveTarget = true;

    bool bFollowing = bNavMeshReady &&
        Bot->GetMoveStatus() ==
            PlayerAIMoveStatusMoving();

    // Only (re)issue when the goal moved or the follower went idle -
    // restarting an in-flight move stutters the pawn.
    const bool bCanIssuePath =
        !AI.bHasIssuedMoveTarget ||
        Now - AI.LastRepathTime >= 0.50f;

    if (bNavMeshReady && bCanIssuePath &&
        (bGoalChanged || !bFollowing))
    {
        AI.LastRepathTime = Now;
        AI.LastIssuedMoveTarget = FVector(Dest);
        AI.bHasIssuedMoveTarget = true;

        // Goal projection first, raw second - some goals sit slightly off
        // the navmesh (chests on props, zone points over water).
        if (Bot->MoveToLocation(FVector(Dest), AcceptRadius, true, true, true, false, (UObject*)nullptr, true) == 0 /*Failed*/)
            Bot->MoveToLocation(FVector(Dest), AcceptRadius, true, true, false, false, (UObject*)nullptr, true);

        bFollowing = Bot->GetMoveStatus() == PlayerAIMoveStatusMoving();
    }

    if (bSprint)
        Sprint(AI);

    // Direct input only when the path follower isn't driving - it keeps bots
    // running where the navmesh has no tiles, without fighting the steering.
    if (!bFollowing && Dist > 1.0)
    {
        FVector Dir = Flat / Dist;
        Pawn->AddMovementInput(Dir, 1.f, true);
    }

    // Look where we're running (combat keeps its own focus).
    if (AI.GetState() != EPlayerAIState::EngagingEnemy && Dist > 1.0)
    {
        FVector Dir = Flat / Dist;
        Bot->K2_SetFocalPoint(FVector(Loc.X + Dir.X * 2000.0, Loc.Y + Dir.Y * 2000.0, Loc.Z + 50.0));
    }
}

void NativePlayerAIBackend::StopMove(PlayerAIController& AI)
{
    auto Bot = GetBotController(AI);

    if (Bot)
        Bot->StopMovement();

    AI.bHasIssuedMoveTarget = false;
    AI.LastRepathTime = 0.f;
}

void NativePlayerAIBackend::SetFocalPoint(PlayerAIController& AI, const FVector& Point)
{
    auto Bot = GetBotController(AI);

    if (Bot)
        Bot->K2_SetFocalPoint(FVector(Point));
}

void NativePlayerAIBackend::ClearFocus(PlayerAIController& AI)
{
    auto Bot = GetBotController(AI);

    if (Bot)
        Bot->K2_ClearFocus();
}

void NativePlayerAIBackend::StartFire(PlayerAIController& AI)
{
    auto Pawn = AI.GetPawn();

    if (Pawn && !AI.bNativeFiring)
    {
        AI.bNativeFiring = true;
        Pawn->PawnStartFire((uint8)0);
    }
}

void NativePlayerAIBackend::StopFire(PlayerAIController& AI)
{
    auto Pawn = AI.GetPawn();

    if (Pawn && AI.bNativeFiring)
    {
        AI.bNativeFiring = false;
        Pawn->PawnStopFire((uint8)0);
    }
}

void NativePlayerAIBackend::Jump(PlayerAIController& AI)
{
    auto Pawn = AI.GetPawn();

    if (Pawn)
        Pawn->Jump();
}

bool NativePlayerAIBackend::SkydiveFrom(PlayerAIController& AI, const FVector& Location)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return false;

    Pawn->K2_TeleportTo(FVector(Location), FRotator{}, false, true);
    ReplicationBehavior::PushTeleportUpdate(Pawn);

    AI.SetTransitionDamageProtection(true);

    return VersionFeatureAdapter::TryBeginSkydiving(Pawn);
}
