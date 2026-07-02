#include "pch.h"
// ============================================================================
// Magnesium PlayerAI - Magnesium integration modules
//   MagnesiumPlayerAIConfig, MagnesiumPlayerAISpawner,
//   MagnesiumPlayerAIFillManager, MagnesiumPlayerAIIntegration
// ============================================================================
#include "../Public/MagnesiumPlayerAIConfig.h"
#include "../Public/MagnesiumPlayerAISettings.h"
#include "../Public/MagnesiumPlayerAISpawner.h"
#include "../Public/MagnesiumPlayerAIFillManager.h"
#include "../Public/MagnesiumPlayerAIIntegration.h"
#include "../Public/PlayerAIManager.h"
#include "../Public/PlayerAIConfig.h"
#include "../Public/PlayerAIFaultGuard.h"
#include "../Public/AIDebugLogger.h"
#include "../Public/AINameGenerator.h"
#include "../Public/VersionFeatureAdapter.h"
#include "../Public/ReplicationBehavior.h"
#include "../Public/LandingBehavior.h"
#include "../../Public/Configuration.h"
#include "../../Public/GUI.h"
#include "../../Public/Misc.h"
#include "../../../FortniteGame/Public/FortGameMode.h"
#include "../../../Engine/Public/NetDriver.h"
#include <unordered_set>

// Existing Magnesium symbols reused by the PlayerAI spawn pipeline (defined
// in FortGameMode.cpp). Reading them keeps PlayerAI players consistent with
// real player registration without modifying any existing system.
extern uint64_t NotifyGameMemberAdded_;
extern int16_t WorldPlayerId;

// ============================================================================
// MagnesiumPlayerAIConfig
// ============================================================================

int MagnesiumPlayerAIConfig::GetMaxPlayerCount()
{
    return VersionFeatureAdapter::GetMaxPlayerCount();
}

int MagnesiumPlayerAIConfig::ComputeDesiredPlayerAICount(int RealPlayerCount)
{
    if (RealPlayerCount <= 0)
        return 0; // PlayerAI players fill only after a real player joined

    const int MaxPlayers = GetMaxPlayerCount();

    // Non-AI participants that already occupy lobby slots: real players and
    // any pawns from the existing bot command (which stays untouched).
    int CommandBotCount = 0;
    auto GameState = VersionFeatureAdapter::GetGameState();

    if (GameState)
    {
        for (auto& PlayerState : GameState->PlayerArray)
        {
            if (!PlayerState)
                continue;

            if (!(PlayerState->HasbIsABot() && PlayerState->bIsABot))
                continue;

            auto Owner = PlayerState->HasOwner() && PlayerState->Owner ? PlayerState->Owner->Cast<AFortPlayerControllerAthena>() : nullptr;

            if (!PlayerAIManager::IsPlayerAI(Owner))
                CommandBotCount++;
        }
    }

    int Desired = MaxPlayers - RealPlayerCount - CommandBotCount;

    if (Desired < 0)
        Desired = 0;

    if (Desired > PlayerAIConfig::AbsoluteMaxPlayerAIs)
        Desired = PlayerAIConfig::AbsoluteMaxPlayerAIs;

    return Desired;
}

// ============================================================================
// MagnesiumPlayerAISpawner
// ============================================================================

static bool PlayerAIFindSpawnTransform(FTransform& OutTransform)
{
    // Pre-match island player starts (includes warmup starts on every
    // supported version).
    static auto StartClass = FindClass("PlayerStart");

    if (StartClass)
    {
        TArray<AActor*> Starts;
        Utils::GetAll(StartClass, Starts);

        if (Starts.Num() > 0)
        {
            auto Start = Starts[rand() % Starts.Num()];

            if (Start)
            {
                OutTransform = Start->GetTransform();
                Starts.Free();
                return true;
            }
        }

        Starts.Free();
    }

    // Fallback 1: next to an existing participant.
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    if (GameMode && GameMode->AlivePlayers.Num() > 0)
    {
        auto Other = (AFortPlayerControllerAthena*)GameMode->AlivePlayers[0];
        auto OtherPawn = Other ? (Other->HasMyFortPawn() && Other->MyFortPawn ? Other->MyFortPawn : Other->Pawn) : nullptr;

        if (OtherPawn)
        {
            FVector Loc = OtherPawn->K2_GetActorLocation();
            Loc.X += PlayerAIRandRange(-1500.f, 1500.f);
            Loc.Y += PlayerAIRandRange(-1500.f, 1500.f);
            Loc.Z += 100.f;
            OutTransform = FTransform(Loc);
            AIDebugLogger::MissingFeature("PlayerStartData", "spawning PlayerAI near existing players");
            return true;
        }
    }

    // Fallback 2: map center ground.
    auto GameState = VersionFeatureAdapter::GetGameState();

    if (GameState && GameState->HasMapInfo() && GameState->MapInfo)
    {
        FVector Center = GameState->MapInfo->GetMapCenter();
        bool bFound = false;
        FVector Ground = VersionFeatureAdapter::FindGroundLocation(Center, bFound);

        if (!bFound)
            Ground.Z += 5000.f;
        else
            Ground.Z += 100.f;

        OutTransform = FTransform(Ground);
        AIDebugLogger::MissingFeature("PlayerStartData", "spawning PlayerAI at the map center");
        return true;
    }

    return false;
}

// Registers the entity in the same match participation structures real
// players use so damage / eliminations / kill credit / alive counts / win
// conditions work natively.
static bool PlayerAIRegisterMatchParticipant(AFortPlayerControllerAthena* PC, AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn, FTransform& Transform)
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();
    auto GameState = VersionFeatureAdapter::GetGameState();

    if (!GameMode || !GameState)
        return false;

    // TODO: connect this to the Magnesium match phase system if team
    //       assignment rules change for future playlists.
    PlayerState->TeamIndex = AFortGameMode::PickTeam(GameMode, 0, PC);

    if (PlayerState->HasSquadId())
        PlayerState->SquadId = PlayerState->TeamIndex - 3;

    // Marked as a simulated (non-human) participant for the engine; this is
    // unrelated to the bot command and required so the native player list
    // logic treats the AI as server controlled.
    if (PlayerState->HasbIsABot())
        PlayerState->bIsABot = true;

    if (PlayerState->HasWorldPlayerId())
        PlayerState->WorldPlayerId = WorldPlayerId++;

    if (GameState->HasGameMemberInfoArray())
    {
        auto Member = (FGameMemberInfo*)malloc(FGameMemberInfo::Size());

        if (Member)
        {
            memset((PBYTE)Member, 0, FGameMemberInfo::Size());

            Member->MostRecentArrayReplicationKey = -1;
            Member->ReplicationID = -1;
            Member->ReplicationKey = -1;
            Member->TeamIndex = PlayerState->TeamIndex;
            Member->SquadId = PlayerState->HasSquadId() ? PlayerState->SquadId : 0;
            Member->MemberUniqueId = PlayerState->HasUniqueID() ? PlayerState->UniqueID : PlayerState->UniqueId;

            auto& NewMember = GameState->GameMemberInfoArray.Members.Add(*Member, FGameMemberInfo::Size());
            GameState->GameMemberInfoArray.MarkItemDirty(NewMember);

            auto NotifyGameMemberAdded = (void(*)(AFortGameStateAthena*, uint8_t, uint8_t, FUniqueNetIdRepl*)) NotifyGameMemberAdded_;

            if (NotifyGameMemberAdded)
                NotifyGameMemberAdded(GameState, Member->SquadId, Member->TeamIndex, &Member->MemberUniqueId);

            free(Member);
        }
    }

    // Same ability sets real players receive.
    if (PlayerState->HasAbilitySystemComponent() && PlayerState->AbilitySystemComponent)
    {
        for (auto& AbilitySet : AFortGameMode::AbilitySets)
        {
            if (AbilitySet)
                PlayerState->AbilitySystemComponent->GiveAbilitySet(AbilitySet);
        }
    }

    // Player inventory through the existing Magnesium inventory system.
    if (!PC->WorldInventory)
    {
        PC->WorldInventory = (AFortInventory*)UWorld::SpawnActor(AFortInventory::StaticClass(), Transform);

        if (PC->WorldInventory)
        {
            PC->WorldInventory->SetOwner(PC);
            PC->WorldInventory->InventoryType = 0;
        }
    }

    if (!PC->WorldInventory)
    {
        AIDebugLogger::Error("Spawner", "failed to create a PlayerAI inventory");
        return false;
    }

    PC->bHasInitializedWorldInventory = true;

    // Count as a real match participant: alive players list + players left.
    // TODO: connect this to the Magnesium alive count system if custom
    //       team-based counting is added later.
    GameState->PlayersLeft++;
    GameState->OnRep_PlayersLeft();
    GameMode->AlivePlayers.Add(PC);

    return true;
}

static void PlayerAIGiveStartingItems(AFortPlayerControllerAthena* PC)
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    if (!PC || !PC->WorldInventory)
        return;

    static auto DefaultPickaxe = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");

    if (DefaultPickaxe)
        PC->WorldInventory->GiveItem(DefaultPickaxe);
    else
        AIDebugLogger::MissingFeature("DefaultPickaxe", "PlayerAI spawns without a pickaxe");

    static auto SmartItemDefClass = FindClass("FortSmartBuildingItemDefinition");

    if (GameMode && GameMode->HasStartingItems())
    {
        for (int i = 0; i < GameMode->StartingItems.Num(); i++)
        {
            auto& StartingItem = GameMode->StartingItems.Get(i, FItemAndCount::Size());

            if (StartingItem.Count && StartingItem.Item && (!SmartItemDefClass || !StartingItem.Item->IsA(SmartItemDefClass)))
                PC->WorldInventory->GiveItem(StartingItem.Item, StartingItem.Count);
        }
    }

    // Equip the pickaxe (same flow the player equip path uses).
    if (VersionInfo.FortniteVersion > 3 && PC->WorldInventory->Inventory.ReplicatedEntries.Num() > 0)
    {
        for (int e = 0; e < PC->WorldInventory->Inventory.ReplicatedEntries.Num(); e++)
        {
            auto Entry = (FFortItemEntry*)((PBYTE)PC->WorldInventory->Inventory.ReplicatedEntries.GetData() + (e * FFortItemEntry::Size()));

            if (!Entry || !Entry->ItemDefinition)
                continue;

            if (Entry->ItemDefinition->IsA<UFortWeaponMeleeItemDefinition>())
            {
                PC->ServerExecuteInventoryItem(Entry->ItemGuid);
                PC->ClientEquipItem(Entry->ItemGuid, true);
                break;
            }
        }
    }
}

static void PlayerAIApplyDisplayName(AFortPlayerControllerAthena* PC, AFortPlayerStateAthena* PlayerState, const std::string& Name)
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    std::wstring Wide(Name.begin(), Name.end());
    FString DisplayName = FString(Wide.c_str());

    if (std::floor(VersionInfo.FortniteVersion) < 9)
    {
        PC->ServerChangeName(DisplayName);
    }
    else if (GameMode)
    {
        GameMode->ChangeName(PC, DisplayName, true);
    }

    if (PlayerState)
        PlayerState->OnRep_PlayerName();
}

PlayerAIController* MagnesiumPlayerAISpawner::SpawnOne()
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();
    auto GameState = VersionFeatureAdapter::GetGameState();

    if (!GameMode || !GameState)
    {
        AIDebugLogger::Error("Spawner", "game mode/state unavailable - PlayerAI spawn skipped");
        return nullptr;
    }

    FTransform Transform{};

    if (!PlayerAIFindSpawnTransform(Transform))
    {
        AIDebugLogger::Error("Spawner", "no usable spawn location - PlayerAI spawn skipped");
        return nullptr;
    }

    // Player pawn class: the same class real players use.
    // TODO: connect this to the Magnesium PlayerAI spawning system if a
    //       custom pawn subclass is ever preferred.
    auto PawnClass = GameMode->HasDefaultPawnClass() && GameMode->DefaultPawnClass ? GameMode->DefaultPawnClass : FindObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");

    if (!PawnClass)
    {
        AIDebugLogger::Error("Spawner", "no player pawn class available - PlayerAI spawn skipped");
        return nullptr;
    }

    static auto ControllerClass = FindObject<UClass>(L"/Game/Athena/Athena_PlayerController.Athena_PlayerController_C");

    if (!ControllerClass)
    {
        AIDebugLogger::Error("Spawner", "no player controller class available - PlayerAI spawn skipped");
        return nullptr;
    }

    auto Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(PawnClass, Transform);
    auto PC = (AFortPlayerControllerAthena*)UWorld::SpawnActor(ControllerClass, Transform);

    if (!PC || !Pawn)
    {
        if (Pawn)
            Pawn->K2_DestroyActor();
        if (PC)
            PC->K2_DestroyActor();

        AIDebugLogger::Error("Spawner", "pawn/controller spawn failed - PlayerAI spawn skipped");
        return nullptr;
    }

    PC->Possess(Pawn);
    PC->MyFortPawn = Pawn;

    auto PlayerState = (AFortPlayerStateAthena*)UWorld::SpawnActor(AFortPlayerStateAthena::StaticClass(), Transform);

    if (!PlayerState)
    {
        Pawn->K2_DestroyActor();
        PC->K2_DestroyActor();
        AIDebugLogger::Error("Spawner", "player state spawn failed - PlayerAI spawn skipped");
        return nullptr;
    }

    PlayerState->SetOwner(PC);
    PC->PlayerState = PlayerState;
    PC->OnRep_PlayerState();
    Pawn->PlayerState = PlayerState;
    Pawn->OnRep_PlayerState();

    // Normal player health/shield values - PlayerAI uses the existing
    // damage pipeline, no special stats.
    Pawn->SetMaxHealth(100.f);
    Pawn->SetHealth(100.f);
    Pawn->SetMaxShield(100.f);
    Pawn->SetShield(0.f);

    ReplicationBehavior::SetupPawnReplication(Pawn);

    if (!PlayerAIRegisterMatchParticipant(PC, PlayerState, Pawn, Transform))
    {
        Pawn->K2_DestroyActor();
        PlayerState->K2_DestroyActor();
        PC->K2_DestroyActor();
        return nullptr;
    }

    VersionFeatureAdapter::ApplyDefaultCosmetics(PlayerState, Pawn);

    const std::string Name = AINameGenerator::NextName();
    PlayerAIApplyDisplayName(PC, PlayerState, Name);

    PlayerAIGiveStartingItems(PC);

    PlayerAIEntity Entity{};
    Entity.PC = PC;
    Entity.PlayerState = PlayerState;
    Entity.DisplayName = Name;

    return PlayerAIManager::RegisterEntity(Entity);
}

AFortPlayerPawnAthena* MagnesiumPlayerAISpawner::SpawnPawnAt(PlayerAIController& AI, const FVector& Location, bool bGround)
{
    auto PC = AI.Entity.PC;
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    if (!PC || !GameMode)
        return nullptr;

    FVector SpawnLoc = Location;

    if (bGround)
    {
        bool bFound = false;
        FVector Ground = VersionFeatureAdapter::FindGroundLocation(Location, bFound);

        if (bFound)
            SpawnLoc = Ground;

        SpawnLoc.Z += 150.f;
    }

    auto OldPawn = AI.GetPawn();

    if (OldPawn)
        OldPawn->K2_DestroyActor();

    auto PawnClass = GameMode->HasDefaultPawnClass() && GameMode->DefaultPawnClass ? GameMode->DefaultPawnClass : FindObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");

    if (!PawnClass)
        return nullptr;

    FTransform Transform(SpawnLoc);
    auto Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(PawnClass, Transform);

    if (!Pawn)
    {
        AIDebugLogger::Error("Spawner", "%s pawn respawn failed", AI.Entity.DisplayName.c_str());
        return nullptr;
    }

    PC->Possess(Pawn);
    PC->MyFortPawn = Pawn;

    if (AI.Entity.PlayerState)
    {
        Pawn->PlayerState = AI.Entity.PlayerState;
        Pawn->OnRep_PlayerState();
    }

    Pawn->SetMaxHealth(100.f);
    Pawn->SetHealth(100.f);
    Pawn->SetMaxShield(100.f);

    ReplicationBehavior::SetupPawnReplication(Pawn);
    VersionFeatureAdapter::ApplyDefaultCosmetics(AI.Entity.PlayerState, Pawn);

    AI.CachedGroundZ = (float)SpawnLoc.Z - 150.f;

    // Re-equip the pickaxe / best weapon after the new pawn exists.
    PlayerAIGiveStartingItems(PC);

    return Pawn;
}

void MagnesiumPlayerAISpawner::DespawnEntity(PlayerAIController& AI, const char* Reason)
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();
    auto GameState = VersionFeatureAdapter::GetGameState();
    auto PC = AI.Entity.PC;

    AIDebugLogger::Verbose("Spawner", "despawning AIPlayer '%s' (%s)", AI.Entity.DisplayName.c_str(), Reason ? Reason : "");

    // Only decrement live match counters when the AI still occupied a slot
    // (dead AI already left the alive counts through the native pipeline).
    const bool bWasAlive = AI.IsAlive();

    auto Pawn = AI.GetPawn();

    if (Pawn)
        Pawn->K2_DestroyActor();

    if (PC && GameMode)
    {
        for (int i = 0; i < GameMode->AlivePlayers.Num(); i++)
        {
            if (GameMode->AlivePlayers[i] == PC)
            {
                GameMode->AlivePlayers.Remove(i);

                if (bWasAlive && GameState && GameState->PlayersLeft > 0)
                {
                    GameState->PlayersLeft--;
                    GameState->OnRep_PlayersLeft();
                }
                break;
            }
        }
    }

    // NOTE: the GameMemberInfoArray entry is intentionally left in place -
    // removing fast array entries is version fragile and a stale pre-match
    // member entry is harmless.
    // TODO: connect this to the Magnesium replication system if per-version
    //       member removal is added later.

    if (PC && PC->WorldInventory)
        PC->WorldInventory->K2_DestroyActor();

    if (AI.Entity.PlayerState)
        AI.Entity.PlayerState->K2_DestroyActor();

    if (PC)
        PC->K2_DestroyActor();

    AI.Entity.PC = nullptr;
    AI.Entity.PlayerState = nullptr;
    AI.bDeathHandled = true;
}

// ============================================================================
// MagnesiumPlayerAIFillManager
// ============================================================================

void MagnesiumPlayerAIFillManager::Reset()
{
    bFillStarted = false;
    FillAllowedAtTime = 0.f;
}

void MagnesiumPlayerAIFillManager::OnRealPlayerJoined(float Now, int RealPlayerCount)
{
    if (!PlayerAIManager::bInitialized)
        return;

    AIDebugLogger::Log("Fill", "real player joined (%d real players)", RealPlayerCount);

    // Real players always have priority: free slots immediately when the
    // lobby is at capacity.
    const int Desired = MagnesiumPlayerAIConfig::ComputeDesiredPlayerAICount(RealPlayerCount);
    int Current = PlayerAIManager::GetTotalCount();

    while (Current > Desired && PlayerAIManager::RemoveOnePreMatch())
        Current--;
}

void MagnesiumPlayerAIFillManager::OnRealPlayerLeft(float Now, int RealPlayerCount)
{
    if (!PlayerAIManager::bInitialized)
        return;

    AIDebugLogger::Log("Fill", "real player left (%d real players)", RealPlayerCount);
}

// Last-resort spawn fault containment: a faulting native call during a
// PlayerAI spawn must never crash the gameserver. Repeated faults disable
// further spawning for the session (logged), everything else keeps running.
// (Kept free of unwindable C++ objects so SEH is allowed here.)
static bool PlayerAITrySpawnOne(PlayerAIController** OutController)
{
    GPlayerAIGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        *OutController = MagnesiumPlayerAISpawner::SpawnOne();
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GPlayerAIGuardedNativeCallDepth--;
    return bOk;
}

static int PlayerAISpawnFaults = 0;
static bool bPlayerAISpawningDisabled = false;

void MagnesiumPlayerAIFillManager::Tick(float Now, int RealPlayerCount, bool bAnyRealPlayerSpawned, EPlayerAIMatchPhase Phase)
{
    if (!PlayerAIManager::bInitialized)
        return;

    // Lobby fill only happens pre-match; during/after the match the current
    // PlayerAI roster plays it out.
    if (Phase != EPlayerAIMatchPhase::PreMatch)
        return;

    if (RealPlayerCount <= 0)
        return;

    if (!bFillStarted)
    {
        // Arm the fill only once the first real player actually stands on
        // the pre-match island - never while a client is still logging in.
        if (!bAnyRealPlayerSpawned)
            return;

        bFillStarted = true;
        FillAllowedAtTime = Now + PlayerAIConfig::FillStartDelaySeconds;
        AIDebugLogger::Log("Fill", "lobby fill armed - target %d total players", MagnesiumPlayerAIConfig::GetMaxPlayerCount());
    }

    if (Now < FillAllowedAtTime)
        return;

    const int Desired = MagnesiumPlayerAIConfig::ComputeDesiredPlayerAICount(RealPlayerCount);
    const int Current = PlayerAIManager::GetTotalCount();

    if (Current < Desired && !bPlayerAISpawningDisabled)
    {
        const int ToSpawn = (Desired - Current) < PlayerAIConfig::MaxSpawnsPerTick ? (Desired - Current) : PlayerAIConfig::MaxSpawnsPerTick;

        for (int i = 0; i < ToSpawn; i++)
        {
            PlayerAIController* NewController = nullptr;

            if (!PlayerAITrySpawnOne(&NewController))
            {
                PlayerAISpawnFaults++;
                AIDebugLogger::Error("Fill", "PlayerAI spawn faulted (%d/3) - contained", PlayerAISpawnFaults);

                if (PlayerAISpawnFaults >= 3)
                {
                    bPlayerAISpawningDisabled = true;
                    AIDebugLogger::Error("Fill", "PlayerAI spawning disabled after repeated faults - existing PlayerAI keep running");
                }
                break;
            }

            if (!NewController)
                break; // logged inside; retry next tick
        }
    }
    else if (Current > Desired)
    {
        // More real players joined: make room (never exceed max players).
        int Excess = Current - Desired;

        while (Excess > 0 && PlayerAIManager::RemoveOnePreMatch())
            Excess--;
    }
}

// ============================================================================
// MagnesiumPlayerAIIntegration
// ============================================================================

static std::unordered_set<void*> KnownRealPlayerControllers;

int MagnesiumPlayerAIIntegration::CountRealPlayers(UNetDriver* Driver)
{
    if (!Driver)
        return 0;

    int Count = 0;

    for (int i = 0; i < Driver->ClientConnections.Num(); i++)
    {
        auto Connection = Driver->ClientConnections[i];

        if (Connection && Connection->PlayerController && Connection->PlayerController->PlayerState)
            Count++;
    }

    return Count;
}

// True once at least one real human player has a pawn in the world (i.e.
// finished logging in and spawned onto the pre-match island).
static bool PlayerAIAnyRealPlayerSpawned(UNetDriver* Driver)
{
    if (!Driver)
        return false;

    for (int i = 0; i < Driver->ClientConnections.Num(); i++)
    {
        auto Connection = Driver->ClientConnections[i];

        if (!Connection || !Connection->PlayerController)
            continue;

        auto PC = Connection->PlayerController;
        auto Pawn = PC->HasMyFortPawn() && PC->MyFortPawn ? PC->MyFortPawn : PC->Pawn;

        if (Pawn)
            return true;
    }

    return false;
}

void MagnesiumPlayerAIIntegration::OnGameserverConfigLoaded()
{
    AIDebugLogger::Log("Lifecycle", "OnGameserverConfigLoaded (Enable AIs: %s)", MagnesiumPlayerAISettings::bEnableAIs ? "ON" : "OFF");
}

void MagnesiumPlayerAIIntegration::OnGameserverStarted()
{
    AIDebugLogger::Log("Lifecycle", "OnGameserverStarted - initializing the PlayerAI system");
    PlayerAIManager::Initialize();
    MagnesiumPlayerAIFillManager::Reset();
}

void MagnesiumPlayerAIIntegration::OnMatchCreated()
{
    AIDebugLogger::Log("Lifecycle", "OnMatchCreated");
}

void MagnesiumPlayerAIIntegration::OnRealPlayerJoined(int RealPlayerCount)
{
    AIDebugLogger::Log("Lifecycle", "OnRealPlayerJoined (%d real players)", RealPlayerCount);
    MagnesiumPlayerAIFillManager::OnRealPlayerJoined(VersionFeatureAdapter::GetTimeSeconds(), RealPlayerCount);
}

void MagnesiumPlayerAIIntegration::OnRealPlayerLeft(int RealPlayerCount)
{
    AIDebugLogger::Log("Lifecycle", "OnRealPlayerLeft (%d real players)", RealPlayerCount);
    MagnesiumPlayerAIFillManager::OnRealPlayerLeft(VersionFeatureAdapter::GetTimeSeconds(), RealPlayerCount);
}

void MagnesiumPlayerAIIntegration::OnPreMatchStarted()
{
    AIDebugLogger::Log("Lifecycle", "OnPreMatchStarted - waiting for a real player before filling the lobby");
}

void MagnesiumPlayerAIIntegration::OnTransportPhaseStarted()
{
    AIDebugLogger::Log("Lifecycle", "OnTransportPhaseStarted (%d PlayerAI players in the match)", PlayerAIManager::GetTotalCount());
    LandingBehavior::BuildClusters(PlayerAIManager::GetWorld());
}

void MagnesiumPlayerAIIntegration::OnMatchEnded()
{
    AIDebugLogger::Log("Lifecycle", "OnMatchEnded");
    PlayerAIManager::OnMatchEnded();
    MagnesiumPlayerAIFillManager::Reset();
}

void MagnesiumPlayerAIIntegration::OnGameserverShutdown()
{
    AIDebugLogger::Log("Lifecycle", "OnGameserverShutdown");
    PlayerAIManager::Shutdown("gameserver shutdown");
    MagnesiumPlayerAIFillManager::Reset();
    KnownRealPlayerControllers.clear();
}

const char* MagnesiumPlayerAIIntegration::GetStatusLine()
{
    return StatusLine;
}

void MagnesiumPlayerAIIntegration::OnServerTickInternal(UNetDriver* Driver, float DeltaSeconds)
{
    auto World = UWorld::GetWorld();

    if (!World || !Driver || Driver != World->NetDriver)
        return;

    const float Now = VersionFeatureAdapter::GetTimeSeconds();

    // ---- Lifecycle: config loaded ----
    if (!bConfigLoadedFired && FConfiguration::bReadyToStart)
    {
        bConfigLoadedFired = true;
        OnGameserverConfigLoaded();
    }

    // ---- Lifecycle: gameserver started / joinable ----
    if (MagnesiumPlayerAISettings::bEnableAIs && !PlayerAIManager::bInitialized && GUI::gsStatus >= Joinable && Misc::bHookedAll)
    {
        if (!bServerStartedFired)
        {
            bServerStartedFired = true;
            OnGameserverStarted();
        }
    }

    if (!PlayerAIManager::bInitialized)
        return;

    // Toggled off while still pre-match: return to stock behavior.
    if (!MagnesiumPlayerAISettings::bEnableAIs && VersionFeatureAdapter::GetMatchPhase() <= EPlayerAIMatchPhase::PreMatch)
    {
        PlayerAIManager::Shutdown("Enable AIs turned off");
        MagnesiumPlayerAIFillManager::Reset();
        bServerStartedFired = false;
        snprintf(StatusLine, sizeof(StatusLine), "PlayerAI: disabled");
        return;
    }

    // ---- Lifecycle: match created ----
    if (!bMatchCreatedFired && VersionFeatureAdapter::GetGameMode() && VersionFeatureAdapter::GetGameState())
    {
        bMatchCreatedFired = true;
        OnMatchCreated();
    }

    const auto Phase = VersionFeatureAdapter::GetMatchPhase();

    // ---- Lifecycle: pre-match / transport / match end ----
    if (!bPreMatchFired && Phase == EPlayerAIMatchPhase::PreMatch)
    {
        bPreMatchFired = true;
        OnPreMatchStarted();
    }

    if (!bTransportFired && Phase == EPlayerAIMatchPhase::Transport)
    {
        bTransportFired = true;
        OnTransportPhaseStarted();
    }

    if (!bMatchEndedFired && Phase == EPlayerAIMatchPhase::Ended)
    {
        bMatchEndedFired = true;
        OnMatchEnded();
        snprintf(StatusLine, sizeof(StatusLine), "PlayerAI: match ended (%d were eliminated)", PlayerAIManager::GetEliminatedCount());
        return;
    }

    if (bMatchEndedFired)
        return;

    // ---- Real player join/leave detection ----
    const int RealPlayers = CountRealPlayers(Driver);

    if (RealPlayers > LastRealPlayerCount)
        OnRealPlayerJoined(RealPlayers);
    else if (RealPlayers < LastRealPlayerCount)
        OnRealPlayerLeft(RealPlayers);

    LastRealPlayerCount = RealPlayers;

    // ---- Lobby fill + AI updates ----
    MagnesiumPlayerAIFillManager::Tick(Now, RealPlayers, PlayerAIAnyRealPlayerSpawned(Driver), Phase);
    PlayerAIManager::UpdateAll(Now, DeltaSeconds);

    // ---- UI status line (cheap, ~1/sec) ----
    static float LastStatusTime = 0.f;

    if (Now - LastStatusTime > 1.f)
    {
        LastStatusTime = Now;
        snprintf(StatusLine, sizeof(StatusLine), "PlayerAI: %d spawned, %d alive, %d eliminated (%s)",
            PlayerAIManager::GetTotalCount(), PlayerAIManager::GetAliveCount(),
            PlayerAIManager::GetEliminatedCount(), PlayerAIMatchPhaseToString(Phase));
    }
}

// (Kept free of unwindable C++ objects so SEH is allowed here. The depth
// counter tells the vectored crash reporter to defer to this handler.)
bool MagnesiumPlayerAIIntegration::TryServerTick(UNetDriver* Driver, float DeltaSeconds)
{
    GPlayerAIGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        OnServerTickInternal(Driver, DeltaSeconds);
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GPlayerAIGuardedNativeCallDepth--;
    return bOk;
}

void MagnesiumPlayerAIIntegration::OnServerTick(UNetDriver* Driver, float DeltaSeconds)
{
    static int SystemFaults = 0;
    static bool bSystemDisabled = false;

    if (bSystemDisabled)
        return;

    // Fully disabled and never initialized: Magnesium behaves exactly like
    // it does without the PlayerAI system (zero cost, no guard entered).
    if (!MagnesiumPlayerAISettings::bEnableAIs && !PlayerAIManager::bInitialized)
        return;

    if (TryServerTick(Driver, DeltaSeconds))
        return;

    // Last resort: a fault escaped every targeted guard. Contain it, and
    // after repeated faults switch the whole PlayerAI system off for this
    // session rather than ever crashing the gameserver.
    SystemFaults++;
    AIDebugLogger::Error("Integration", "PlayerAI server tick faulted (%d/5) - contained", SystemFaults);

    if (SystemFaults >= 5)
    {
        bSystemDisabled = true;
        snprintf(StatusLine, sizeof(StatusLine), "PlayerAI: disabled after repeated faults");
        AIDebugLogger::Error("Integration", "PlayerAI system disabled for this session to protect the gameserver");
    }
}
