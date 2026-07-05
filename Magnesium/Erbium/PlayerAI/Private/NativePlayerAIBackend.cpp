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
#include "../../../FortniteGame/Public/FortKismetLibrary.h"
#include "../../../Engine/Public/AbilitySystemComponent.h"

// ============================================================================
// PlayerAIEntity::GetPawn (declared in PlayerAIEntity.h)
// ============================================================================

AFortPlayerPawnAthena* PlayerAIEntity::GetPawn() const
{
    if (bNativeBacked)
    {
        auto Bot = (AFortAthenaAIBotController*)NativeController;

        if (!Bot)
            return nullptr;

        return Bot->Pawn;
    }

    if (!PC)
        return nullptr;

    auto Pawn = PC->MyFortPawn;

    if (!Pawn)
        Pawn = PC->Pawn;

    return Pawn;
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

static const UClass* PlayerAIFindClassObject(const char* ShortName)
{
    auto Object = TUObjectArray::FindFirstObject(ShortName);

    if (Object && Object->IsA<UClass>())
        return (const UClass*)Object;

    return nullptr;
}

// Fault-isolated asset class loading: loading a nonexistent path can fault
// on some builds, and a fault during detection must NEVER take the whole
// PlayerAI system down. (Kept free of unwindable C++ objects for SEH.)
static const UClass* PlayerAITryLoadClass(const wchar_t* Path, bool* bOutFaulted)
{
    GPlayerAIGuardedNativeCallDepth++;

    const UClass* Result = nullptr;
    bool bFaulted = false;

    __try
    {
        Result = FindObject<UClass>(Path);
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

// Detection must FORCE-LOAD the Phoebe blueprints: they are lazy loaded and
// are usually not in memory until something references them, so a memory
// scan alone always misses them on a fresh server.
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

    auto BotControllerClass = FindClass("FortAthenaAIBotController");
    bool bLoadFaulted = false;

    if (!GPhoebePawnClass)
        GPhoebePawnClass = PlayerAITryLoadClass(L"/Game/Athena/AI/Phoebe/BP_PlayerPawn_Athena_Phoebe.BP_PlayerPawn_Athena_Phoebe_C", &bLoadFaulted);

    if (!GPhoebePawnClass && !bLoadFaulted)
        GPhoebePawnClass = PlayerAIFindClassObject("BP_PlayerPawn_Athena_Phoebe_C");

    if (!GMutatorClass && !bLoadFaulted)
        GMutatorClass = PlayerAITryLoadClass(L"/Game/Athena/AI/Phoebe/BP_Phoebe_Mutator.BP_Phoebe_Mutator_C", &bLoadFaulted);

    if (!GMutatorClass)
        GMutatorClass = PlayerAIFindClassObject("BP_Phoebe_Mutator_C");

    if (!GMutatorClass)
        GMutatorClass = FindClass("FortAthenaMutator_Bots");

    if (bLoadFaulted)
    {
        // Loading faulted on this build: settle immediately, never retry.
        GNativeAvailable = 0;
        AIDebugLogger::MissingFeature("NativeBotAssetLoading",
            "asset loading faulted - PlayerAI uses the simulated backend");
        return;
    }

    if (BotControllerClass && GPhoebePawnClass && GMutatorClass)
    {
        GNativeAvailable = 1;
        AIDebugLogger::Log("NativeBackend", "native player-bot stack loaded - PlayerAI uses engine bots (navmesh pathing, real weapons)");
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
    GBotMutator = nullptr;
    bNavMeshRequested = false;
    bNavMeshReady = false;
}

bool NativePlayerAIBackend::IsNavMeshReady()
{
    return bNavMeshReady;
}

// ============================================================================
// Navmesh bootstrap
// ============================================================================

// The game ships /Game/Maps/NavMeshBounds: a stub level holding a unit-sized
// NavMeshBoundsVolume for dedicated servers to scale over the map so the
// navmesh generates at runtime. Without nav bounds no tiles are ever built
// and every pathfinding request fails.
static void PlayerAIEnsureNavMesh()
{
    if (bNavMeshReady)
        return;

    auto World = UWorld::GetWorld();

    if (!World || !World->HasNavigationSystem() || !World->NavigationSystem)
        return;

    auto NavSystem = World->NavigationSystem;

    if (!bNavMeshRequested)
    {
        bNavMeshRequested = true;

        auto Statics = UGameplayStatics::GetDefaultObj();
        auto LoadFn = Statics ? Statics->GetFunction("LoadStreamLevel") : nullptr;

        if (!LoadFn)
        {
            AIDebugLogger::MissingFeature("LoadStreamLevel", "navmesh bounds level cannot be streamed - direct-input movement only");
            return;
        }

        // Build the parameter block by property name so this survives layout
        // changes between versions.
        auto Params = LoadFn->GetParamsNamed();
        std::vector<uint8_t> Buffer((size_t)(Params.Size > 0 ? Params.Size : 0x100), 0);

        for (auto& Param : Params.NameOffsetMap)
        {
            if (Param.Name == "WorldContextObject")
                *(UWorld**)(Buffer.data() + Param.Offset) = World;
            else if (Param.Name == "LevelName")
                *(FName*)(Buffer.data() + Param.Offset) = FName(L"/Game/Maps/NavMeshBounds");
            else if (Param.Name == "bMakeVisibleAfterLoad")
                *(bool*)(Buffer.data() + Param.Offset) = true;
            else if (Param.Name == "bShouldBlockOnLoad")
                *(bool*)(Buffer.data() + Param.Offset) = false;
            else if (Param.Name == "LatentInfo")
            {
                // FLatentActionInfo: Linkage(int32), UUID(int32),
                // ExecutionFunction(FName), CallbackTarget(UObject*).
                auto Latent = Buffer.data() + Param.Offset;
                *(int32*)(Latent + 0) = 0;
                *(int32*)(Latent + 4) = 134005; // arbitrary unique UUID
                *(FName*)(Latent + 8) = FName();
                *(UObject**)(Latent + 16) = World;
            }
        }

        Statics->ProcessEvent(LoadFn, Buffer.data());
        AIDebugLogger::Log("NativeBackend", "streaming the NavMeshBounds level for runtime navmesh generation");
        return; // volume appears on a later tick
    }

    // Wait for the volume to stream in, then stretch it over the island.
    static auto VolumeClass = FindClass("NavMeshBoundsVolume");

    if (!VolumeClass)
    {
        AIDebugLogger::MissingFeature("NavMeshBoundsVolume", "no navmesh bounds class - direct-input movement only");
        bNavMeshReady = true; // stop retrying
        return;
    }

    TArray<AActor*> Volumes;
    Utils::GetAll(VolumeClass, Volumes);

    if (Volumes.Num() == 0)
    {
        Volumes.Free();
        return; // still streaming - retry next tick
    }

    auto Volume = Volumes[0];
    Volumes.Free();

    // Cover the whole island.
    Volume->K2_SetActorLocation(FVector(0.f, 0.f, 12000.f), false, nullptr, true);
    Volume->SetActorScale3D(FVector(300000.f, 300000.f, 40000.f));

    // Force dynamic runtime generation on the nav data.
    static auto MainNavDataOffset = NavSystem->GetOffset("MainNavData");

    if (MainNavDataOffset != -1)
    {
        auto NavData = GetFromOffset<AActor*>(NavSystem, MainNavDataOffset);

        if (NavData)
        {
            static auto RuntimeGenerationOffset = NavData->GetOffset("RuntimeGeneration");

            if (RuntimeGenerationOffset != -1)
                GetFromOffset<uint8>(NavData, RuntimeGenerationOffset) = 2; // ERuntimeGenerationType::Dynamic
        }
    }

    auto UpdateFn = NavSystem->GetFunction("OnNavigationBoundsUpdated");

    if (UpdateFn)
        NavSystem->Call<void>(UpdateFn, Volume);

    AIDebugLogger::Log("NativeBackend", "navmesh bounds placed over the island - runtime generation started");
    bNavMeshReady = true;
}

// ============================================================================
// Initialization (mutator + bot manager wiring)
// ============================================================================

void NativePlayerAIBackend::EnsureInitialized()
{
    if (!IsAvailable())
        return;

    PlayerAIEnsureNavMesh();

    if (GBotMutator)
        return;

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

    if (!GBotMutator || !GPhoebePawnClass)
        return Entity;

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
    static int ConsecutiveNullSpawns = 0;

    if (!Pawn)
    {
        AIDebugLogger::Error("NativeBackend", "native bot spawn returned null");

        if (++ConsecutiveNullSpawns >= 3)
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

        if (++ConsecutiveNullSpawns >= 3)
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

        if (++ConsecutiveNullSpawns >= 3)
        {
            GNativeAvailable = 0;
            AIDebugLogger::MissingFeature("NativeBotSpawn",
                "native bot spawns keep coming up broken - simulated backend takes over");
        }

        return Entity;
    }

    ConsecutiveNullSpawns = 0;

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
        static auto DefaultPickaxe = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");

        if (DefaultPickaxe)
        {
            auto Item = Bot->Inventory->GiveItem(DefaultPickaxe);

            if (Item)
                Pawn->EquipWeaponDefinition((UFortItemDefinition*)DefaultPickaxe, Item->ItemEntry.ItemGuid);
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
    return AI.Entity.bNativeBacked ? (AFortAthenaAIBotController*)AI.Entity.NativeController : nullptr;
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

    // One-time sprint ability activation (same path real clients use), then
    // keep the replicated movement style at Sprinting.
    if (!AI.bSprintAbilityActivated)
    {
        AI.bSprintAbilityActivated = true;

        auto PlayerState = AI.Entity.PlayerState;
        auto SprintClass = FindClass("FortGameplayAbility_Sprint");

        if (SprintClass && PlayerState && PlayerState->HasAbilitySystemComponent() && PlayerState->AbilitySystemComponent)
        {
            auto ASC = PlayerState->AbilitySystemComponent;
            auto& Items = ASC->ActivatableAbilities.Items;

            for (int i = 0; i < Items.Num(); i++)
            {
                auto Spec = (FGameplayAbilitySpec*)((PBYTE)Items.GetData() + (i * FGameplayAbilitySpec::Size()));

                if (!Spec || !Spec->Ability || !Spec->Ability->IsA(SprintClass))
                    continue;

                VersionFeatureAdapter::TryActivateAbilityHandle(ASC, Spec->Handle);
                break;
            }
        }
    }

    if (Pawn->HasCurrentMovementStyle())
    {
        static int SprintingValue = -1;

        if (SprintingValue == -1)
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

    const bool bGoalChanged = !AI.bHasMoveTarget || (Dest - AI.MoveTarget).Magnitude() > 500.0;

    AI.MoveTarget = FVector(Dest);
    AI.bHasMoveTarget = true;

    bool bFollowing = Bot->GetMoveStatus() == PlayerAIMoveStatusMoving();

    // Only (re)issue when the goal moved or the follower went idle -
    // restarting an in-flight move stutters the pawn.
    if (bGoalChanged || (!bFollowing && Now - AI.LastRepathTime >= 0.75f))
    {
        AI.LastRepathTime = Now;

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

void NativePlayerAIBackend::SkydiveFrom(PlayerAIController& AI, const FVector& Location)
{
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return;

    Pawn->K2_TeleportTo(FVector(Location), FRotator{}, false, true);
    Pawn->BeginSkydiving(true);
}
