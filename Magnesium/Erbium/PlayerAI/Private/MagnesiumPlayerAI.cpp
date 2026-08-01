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
#include "../Public/NativePlayerAIBackend.h"
#include "../Public/AIDebugLogger.h"
#include "../Public/AINameGenerator.h"
#include "../Public/VersionFeatureAdapter.h"
#include "../Public/ReplicationBehavior.h"
#include "../Public/LandingBehavior.h"
#include "../../Public/Configuration.h"
#include "../../Public/GUI.h"
#include "../../Public/Misc.h"
#include "../../../FortniteGame/Public/FortGameMode.h"
#include "../../../FortniteGame/Public/FortKismetLibrary.h"
#include "../../../Engine/Public/NetDriver.h"
#include <unordered_set>
#include <vector>

std::atomic_bool MagnesiumPlayerAISettings::bEnableAIs{
    false };

// Existing Magnesium world-player sequence reused by the PlayerAI spawn
// pipeline (defined in FortGameMode.cpp).
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

// Player starts are static for a world. The old fallback performed up to
// three GetAllActorsOfClass scans for every fill attempt that briefly missed
// the real player's pawn. Cache one base-class scan per world instead.
static const std::vector<FVector>& PlayerAIGetCachedSpawnStartLocations()
{
    static UWorld* CachedWorld = nullptr;
    static bool bScannedWorld = false;
    static std::vector<FVector> WarmupLocations;
    static std::vector<FVector> GenericLocations;
    static const std::vector<FVector> Empty;

    auto World = UWorld::GetWorld();

    if (!World)
        return Empty;

    if (CachedWorld != World)
    {
        CachedWorld = World;
        bScannedWorld = false;
        WarmupLocations.clear();
        GenericLocations.clear();
    }

    if (!bScannedWorld)
    {
        bScannedWorld = true;

        // Native classes are process-resident. Function-static lookups cache
        // failure too, preventing repeated full reflected-object scans.
        static auto PlayerStartClass =
            FindClass("PlayerStart");
        static auto FortWarmupStartClass =
            FindClass("FortPlayerStartWarmup");
        static auto WarmupStartClass =
            FindClass("PlayerStartWarmup");

        if (PlayerStartClass)
        {
            TArray<AActor*> Starts;
            Utils::GetAll(PlayerStartClass, Starts);

            for (auto Start : Starts)
            {
                if (!Start)
                    continue;

                const FVector Location =
                    Start->K2_GetActorLocation();
                const bool bWarmup =
                    (FortWarmupStartClass &&
                     Start->IsA(FortWarmupStartClass)) ||
                    (WarmupStartClass &&
                     Start->IsA(WarmupStartClass));

                if (bWarmup)
                    WarmupLocations.push_back(Location);
                else
                    GenericLocations.push_back(Location);
            }

            Starts.Free();
        }
    }

    return !WarmupLocations.empty()
        ? WarmupLocations : GenericLocations;
}

static bool PlayerAIFindSpawnTransform(FTransform& OutTransform)
{
    auto IsUsableLocation = [](const FVector& Location)
        {
            return std::isfinite(Location.X) &&
                std::isfinite(Location.Y) &&
                std::isfinite(Location.Z) &&
                fabs(Location.X) < 50000000.0 &&
                fabs(Location.Y) < 50000000.0 &&
                fabs(Location.Z) < 50000000.0;
        };

    // A real player must already be present before fill starts. Spawning
    // near that player's proven warmup location is safer than picking an
    // arbitrary global PlayerStart (which can be a bus, spectator, or
    // kill-volume start on some maps).
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    if (GameMode)
    {
        for (auto Uncasted : GameMode->AlivePlayers)
        {
            auto Other = (AFortPlayerControllerAthena*)Uncasted;

            if (!Other || PlayerAIManager::IsPlayerAI(Other))
                continue;

            auto OtherState =
                (AFortPlayerStateAthena*)Other->PlayerState;

            if (OtherState && OtherState->HasbIsABot() &&
                OtherState->bIsABot)
            {
                continue;
            }

            auto OtherPawn =
                Other->HasMyFortPawn() && Other->MyFortPawn
                ? Other->MyFortPawn
                : Other->Pawn;

            if (!OtherPawn)
                continue;

            FVector Loc = OtherPawn->K2_GetActorLocation();
            const int SpawnSlot =
                PlayerAIManager::GetTotalCount() + 1;
            const double Angle =
                (double)SpawnSlot * 2.399963229728653;
            const double Radius =
                350.0 + sqrt((double)SpawnSlot) * 240.0;
            Loc.X += cos(Angle) * Radius +
                PlayerAIRandRange(-120.f, 120.f);
            Loc.Y += sin(Angle) * Radius +
                PlayerAIRandRange(-120.f, 120.f);

            bool bFoundGround = false;
            FVector Ground =
                VersionFeatureAdapter::FindGroundLocation(
                    Loc, bFoundGround, OtherPawn);

            if (bFoundGround)
                Loc = Ground;

            Loc.Z += 150.f;

            if (!IsUsableLocation(Loc))
                continue;

            OutTransform = FTransform(Loc);
            return true;
        }
    }

    // Cached warmup starts first, then generic PlayerStart locations. The
    // cache performs at most one world actor scan for this map.
    const auto& StartLocations =
        PlayerAIGetCachedSpawnStartLocations();

    if (!StartLocations.empty())
    {
        const int Count = (int)StartLocations.size();
        const int First = rand() % Count;

        for (int Attempt = 0; Attempt < Count; Attempt++)
        {
            FVector Loc =
                StartLocations[(First + Attempt) % Count];
            bool bFoundGround = false;
            FVector Ground =
                VersionFeatureAdapter::FindGroundLocation(
                    Loc, bFoundGround);

            if (bFoundGround)
                Loc = Ground;

            Loc.Z += 150.f;

            if (IsUsableLocation(Loc))
            {
                OutTransform = FTransform(Loc);
                return true;
            }
        }
    }

    // Last resort: grounded map center.
    auto GameState = VersionFeatureAdapter::GetGameState();

    if (GameState && GameState->HasMapInfo() && GameState->MapInfo)
    {
        FVector Center = GameState->MapInfo->GetMapCenter();
        bool bFound = false;
        FVector Ground = VersionFeatureAdapter::FindGroundLocation(Center, bFound);

        if (!bFound)
            return false;

        Ground.Z += 100.f;

        if (!IsUsableLocation(Ground))
            return false;

        OutTransform = FTransform(Ground);
        AIDebugLogger::MissingFeature("PlayerStartData", "spawning PlayerAI at the map center");
        return true;
    }

    return false;
}

static bool PlayerAISetReflectedBool(
    UObject* Object,
    const char* PropertyName,
    bool Value)
{
    if (!Object || !PropertyName)
        return false;

    auto Property =
        Object->GetProperty(PropertyName, 0x20000);

    if (!Property)
        return false;

    const auto Offset = GetFromOffset<uint32>(
        Property, Offsets::Offset_Internal);
    const auto Mask = Property->GetFieldMask();

    if (Offset >= 0x20000)
        return false;

    auto& Byte = GetFromOffset<uint8>(Object, Offset);

    if (Mask)
        Value ? Byte |= Mask : Byte &= ~Mask;
    else
        Byte = Value ? 1 : 0;

    return true;
}

// Real clients acknowledge these loading milestones before native match and
// aircraft cleanup considers them valid participants. A server-owned PlayerAI
// has no connection that can send those RPCs, so complete the same lifecycle
// by reflected name. Missing properties are expected across engine versions.
static void PlayerAIMarkSyntheticParticipantReady(
    AFortPlayerControllerAthena* PC,
    AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn)
{
    if (!PC)
        return;

    PlayerAISetReflectedBool(
        PC, "bHasClientFinishedLoading", true);
    PlayerAISetReflectedBool(
        PC, "bHasServerFinishedLoading", true);
    PlayerAISetReflectedBool(
        PC, "bReadyToStartMatch", true);
    PlayerAISetReflectedBool(
        PC, "bAssignedStartSpawn", true);

    if (Pawn)
    {
        PlayerAISetReflectedBool(
            PC, "bHasInitiallySpawned", true);
        PlayerAISetReflectedBool(
            PC, "bClientPawnIsLoaded", true);
    }

    PlayerAISetReflectedBool(
        PC, "bMarkedAlive", true);

    if (PlayerState)
    {
        PlayerAISetReflectedBool(
            PlayerState, "bIsSpectator", false);
        PlayerAISetReflectedBool(
            PlayerState, "bHasStartedPlaying", true);
        PlayerState->ForceNetUpdate();
    }

    PC->ForceNetUpdate();
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
        // Do not manually shallow-copy FUniqueNetIdRepl into the fast array.
        // It owns backing storage on several versions (including 10.40), and
        // synthetic controllers have no online subsystem to copy it safely.
        // PlayerState/AlivePlayers remain the authoritative replicated data.
        AIDebugLogger::MissingFeature(
            "PlayerAIGameMemberInfo",
            "synthetic member rows are omitted to prevent duplicate names and unsafe UniqueId aliasing");
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

    PlayerAIMarkSyntheticParticipantReady(
        PC, PlayerState, Pawn);

    bool bAlreadyAlive = false;

    for (auto Existing : GameMode->AlivePlayers)
    {
        if (Existing == PC)
        {
            bAlreadyAlive = true;
            break;
        }
    }

    if (!bAlreadyAlive)
        GameMode->AlivePlayers.Add(PC);

    VersionFeatureAdapter::SyncPlayersLeft(true);

    return true;
}

static void PlayerAIGiveStartingItems(AFortPlayerControllerAthena* PC)
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    if (!PC || !PC->WorldInventory)
        return;

    static auto DefaultPickaxe =
        Offsets::StaticFindObject
        ? (const UFortItemDefinition*)SDK::StaticFindObject(
            L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01",
            UFortItemDefinition::StaticClass())
        : nullptr;

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

static void PlayerAIApplyDisplayName(AFortPlayerControllerAthena* PC, AActor* AnyController, AFortPlayerStateAthena* PlayerState, const std::string& Name)
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();

    std::wstring Wide(Name.begin(), Name.end());
    FString DisplayName = FString(Wide.c_str());

    if (PC && std::floor(VersionInfo.FortniteVersion) < 9)
    {
        PC->ServerChangeName(DisplayName);
    }
    else if (GameMode && AnyController)
    {
        // AGameMode::ChangeName takes any controller (native bot
        // controllers included).
        GameMode->ChangeName((AFortPlayerControllerAthena*)AnyController, DisplayName, true);
    }

    // ALWAYS also write the player state name property directly: several
    // versions (seen on 6.x and 10.40) ignore the change for connectionless
    // controllers, which showed every PlayerAI with the same name.
    if (PlayerState)
    {
        std::unordered_set<uint32> WrittenOffsets;
        auto WriteNameProperty = [&](const char* PropertyName)
            {
                auto Off = PlayerState->GetOffset("PlayerNamePrivate");

                if (strcmp(PropertyName, "PlayerNamePrivate") != 0)
                    Off = PlayerState->GetOffset(PropertyName);

                if (Off == (uint32)-1 || Off >= 0x10000 ||
                    WrittenOffsets.count(Off))
                {
                    return false;
                }

                // Construct a separate allocation for each slot. 10.40 has
                // both replicated PlayerNamePrivate and a public PlayerName
                // cache; shallow-copying one FString into both would alias
                // their storage.
                GetFromOffset<FString>(
                    PlayerState, Off) = FString(Wide.c_str());
                WrittenOffsets.insert(Off);
                VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                    PlayerState,
                    strcmp(PropertyName, "PlayerNamePrivate") == 0
                    ? L"PlayerNamePrivate"
                    : L"PlayerName");
                return true;
            };

        const bool bPrivateWritten =
            WriteNameProperty("PlayerNamePrivate");
        const bool bPublicWritten =
            WriteNameProperty("PlayerName");

        if (!bPrivateWritten && !bPublicWritten)
            AIDebugLogger::MissingFeature("PlayerAIDisplayName",
                "no player name property found - PlayerAI names may not display");

        PlayerState->OnRep_PlayerName();
        PlayerState->FlushNetDormancy();
        PlayerState->ForceNetUpdate();

        // Diagnostic readback: GetPlayerName() is what nameplates/killfeed
        // resolve. Compare the whole value so shared prefixes cannot hide a
        // publication failure.
        if (AIDebugLogger::bVerbose)
        {
            FString Applied = PlayerState->GetPlayerName();
            const bool bReadable =
                Applied.Data && Applied.NumElements > 1 &&
                Applied.NumElements < 512;
            const bool bMatches = bReadable &&
                strcmp(
                    Applied.ToString().c_str(),
                    Name.c_str()) == 0;

            AIDebugLogger::Verbose(
                "Names", "%s: private=%d public=%d readback %s",
                Name.c_str(), bPrivateWritten ? 1 : 0,
                bPublicWritten ? 1 : 0,
                bMatches ? "ok" : "MISMATCH");
        }
    }
}

// Resident-only class lookup. Loading a package from a spawn/recovery path
// can block TickFlush indefinitely.
// (Kept free of unwindable C++ objects for SEH.)
static const UClass* PlayerAITryFindLoadedClass(
    const wchar_t* Path)
{
    if (!Offsets::StaticFindObject || !Path)
        return nullptr;

    GPlayerAIGuardedNativeCallDepth++;

    const UClass* Result = nullptr;

    __try
    {
        Result = (const UClass*)SDK::StaticFindObject(
            Path, UClass::StaticClass());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Result = nullptr;
    }

    GPlayerAIGuardedNativeCallDepth--;
    return Result;
}

static const UClass* PlayerAIGetFallbackPawnClass()
{
    // Cache failure as well as success. If the playlist did not publish a
    // default pawn and this class is not resident, repeated fill/recovery
    // attempts must not keep probing reflection from TickFlush.
    static const UClass* PawnClass =
        PlayerAITryFindLoadedClass(
            L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");
    return PawnClass;
}

static const UClass* PlayerAIGetSimulatedControllerClass()
{
    static const UClass* ControllerClass =
        PlayerAITryFindLoadedClass(
            L"/Game/Athena/Athena_PlayerController.Athena_PlayerController_C");
    return ControllerClass;
}

// Fall damage immunity: landings (and clumsy pathing) must never kill a
// PlayerAI. The gameplay effect exists on Chapter 2+ versions; older
// versions are covered by the soft-landing assist instead.
static void PlayerAIApplyFallImmunity(AFortPlayerStateAthena* PlayerState)
{
    if (!PlayerState || !PlayerState->HasAbilitySystemComponent() || !PlayerState->AbilitySystemComponent)
        return;

    // The asset shipped with Chapter 2 - never attempt the load below 11.00.
    if (VersionInfo.FortniteVersion < 11.00)
        return;

    static auto ImmunityGE =
        PlayerAITryFindLoadedClass(
            L"/Game/Athena/Items/Gameplay/BackPacks/Ashton/GE_AshtonPack_FallDamageImmune.GE_AshtonPack_FallDamageImmune_C");

    if (!ImmunityGE)
    {
        AIDebugLogger::MissingFeature("FallDamageImmunityGE", "PlayerAI relies on the soft-landing assist instead");
        return;
    }

    auto ASC = PlayerState->AbilitySystemComponent;
    ASC->BP_ApplyGameplayEffectToSelf(ImmunityGE, 1.f, ASC->MakeEffectContext());
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

    // ---- Native backend first: engine bots (navmesh pathing, native
    // movement/sprint/swimming, real weapons). Falls through to the
    // simulated pipeline when unavailable.
    if (NativePlayerAIBackend::IsAvailable())
    {
        const std::string Name = AINameGenerator::NextName();

        auto Entity = NativePlayerAIBackend::SpawnNativeEntity(Transform, Name);

        if (Entity.IsValid())
        {
            PlayerAIApplyDisplayName(nullptr, Entity.NativeController, Entity.PlayerState, Name);
            VersionFeatureAdapter::ApplyRandomSkin(Entity.PlayerState, Entity.GetPawn());
            PlayerAIApplyFallImmunity(Entity.PlayerState);

            auto Registered =
                PlayerAIManager::RegisterEntity(Entity);
            VersionFeatureAdapter::SyncPlayersLeft(true);
            return Registered;
        }

        // Backend may have disabled itself (missing SpawnBot etc.) - if it
        // is still available this was a transient failure worth retrying.
        if (NativePlayerAIBackend::IsAvailable())
            return nullptr;

        AIDebugLogger::Log("Spawner", "native backend unavailable - using the simulated backend");
    }

    // Player pawn class: the same class real players use.
    // TODO: connect this to the Magnesium PlayerAI spawning system if a
    //       custom pawn subclass is ever preferred.
    auto PawnClass =
        GameMode->HasDefaultPawnClass() &&
        GameMode->DefaultPawnClass
        ? GameMode->DefaultPawnClass
        : PlayerAIGetFallbackPawnClass();

    if (!PawnClass)
    {
        AIDebugLogger::Error("Spawner", "no player pawn class available - PlayerAI spawn skipped");
        return nullptr;
    }

    auto ControllerClass =
        PlayerAIGetSimulatedControllerClass();

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

    // Publish the final unique display name before this PlayerState enters
    // the alive roster (notably important on 10.40).
    const std::string Name = AINameGenerator::NextName();
    PlayerAIApplyDisplayName(PC, PC, PlayerState, Name);

    if (!PlayerAIRegisterMatchParticipant(PC, PlayerState, Pawn, Transform))
    {
        Pawn->K2_DestroyActor();
        PlayerState->K2_DestroyActor();
        PC->K2_DestroyActor();
        return nullptr;
    }

    VersionFeatureAdapter::ApplyRandomSkin(PlayerState, Pawn);
    PlayerAIApplyFallImmunity(PlayerState);

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
    auto Pawn = AI.GetPawn();

    FVector SpawnLoc = Location;

    if (bGround)
    {
        FVector Ground{};

        if (!VersionFeatureAdapter::TryResolveGroundedLandingSpot(
                Location, Pawn, Ground))
        {
            return nullptr;
        }

        SpawnLoc = Ground;
        SpawnLoc.Z += 150.f;
    }

    // Recovery is deliberately non-destructive. Bus/landing fallbacks used
    // to destroy a healthy pawn before proving that RestartPlayer or a new
    // spawn worked, producing the visible teleport/zip followed by death.
    if (Pawn)
    {
        Pawn->K2_TeleportTo(
            SpawnLoc, Pawn->K2_GetActorRotation(), false, true);

        if (Pawn->HasCharacterMovement() && Pawn->CharacterMovement)
            Pawn->CharacterMovement->Velocity = FVector{};

        AI.SetTransitionDamageProtection(true);

        if (bGround)
        {
            if (auto EndSkydiving =
                    Pawn->GetFunction("EndSkydiving"))
            {
                VersionFeatureAdapter::SafeCallNoArgs(
                    Pawn, EndSkydiving);
            }
        }

        ReplicationBehavior::SetupPawnReplication(Pawn);
        ReplicationBehavior::PushTeleportUpdate(Pawn);

        if (bGround)
            AI.CachedGroundZ = (float)SpawnLoc.Z - 150.f;

        AI.PosVertVel = 0.f;
        AI.bPosGrounded = false;
        return Pawn;
    }

    // Native bots are engine-owned and must never be respawned as a player
    // pawn. Their valid pawn is reused above; a missing one means the native
    // lifecycle has already removed it.
    if (AI.Entity.bNativeBacked || !PC || !GameMode)
        return nullptr;

    auto PawnClass =
        GameMode->HasDefaultPawnClass() && GameMode->DefaultPawnClass
        ? GameMode->DefaultPawnClass
        : PlayerAIGetFallbackPawnClass();

    if (!PawnClass)
        return nullptr;

    FTransform Transform(SpawnLoc);
    Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(
        PawnClass, Transform);

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
    Pawn->SetShield(0.f);

    ReplicationBehavior::SetupPawnReplication(Pawn);
    // The persistent PlayerState already owns the build-randomized cosmetic
    // selection from initial spawn. Re-running discovery/customization for a
    // recovery pawn both changed its skin and multiplied bus-exit workload.
    AI.SetTransitionDamageProtection(true);

    if (bGround)
        AI.CachedGroundZ = (float)SpawnLoc.Z - 150.f;

    // Placement data can be a canopy/roof height: swept gravity drops the
    // pawn onto real collision over the next ticks instead of trusting it.
    AI.PosVertVel = 0.f;
    AI.bPosGrounded = false;

    return Pawn;
}

static bool PlayerAIBuildSafeAirborneStart(
    PlayerAIController& AI,
    const FVector& Desired,
    FVector& OutStart)
{
    FVector SafeDesired = Desired;

    if (!std::isfinite(SafeDesired.X) ||
        !std::isfinite(SafeDesired.Y))
    {
        SafeDesired.X = AI.HomeLocation.X;
        SafeDesired.Y = AI.HomeLocation.Y;
    }

    if (!std::isfinite(SafeDesired.X) ||
        !std::isfinite(SafeDesired.Y))
    {
        return false;
    }

    double AnchorZ =
        std::isfinite(SafeDesired.Z)
        ? SafeDesired.Z
        : 0.0;

    if (std::isfinite(AI.HomeLocation.Z) &&
        AI.HomeLocation.Z > AnchorZ)
    {
        AnchorZ = AI.HomeLocation.Z;
    }

    if (AnchorZ < 0.0)
        AnchorZ = 0.0;

    OutStart = SafeDesired;
    bool bUsedActiveAircraft = false;
    auto Aircraft = VersionFeatureAdapter::GetAircraft();

    if (Aircraft &&
        VersionFeatureAdapter::GetMatchPhase() ==
            EPlayerAIMatchPhase::Transport)
    {
        const float Now =
            VersionFeatureAdapter::GetTimeSeconds();
        bool bFlightWindowOpen = true;

        if (Aircraft->HasFlightEndTime() &&
            Aircraft->FlightEndTime > 1.f &&
            Now > Aircraft->FlightEndTime + 1.f)
        {
            bFlightWindowOpen = false;
        }
        else if (!Aircraft->HasFlightEndTime() &&
            Aircraft->HasDropEndTime() &&
            Aircraft->DropEndTime > 1.f &&
            Now > Aircraft->DropEndTime + 5.f)
        {
            bFlightWindowOpen = false;
        }

        if (bFlightWindowOpen)
        {
            FVector AircraftLocation =
                Aircraft->K2_GetActorLocation();

            if (std::isfinite(AircraftLocation.X) &&
                std::isfinite(AircraftLocation.Y) &&
                std::isfinite(AircraftLocation.Z))
            {
                OutStart = AircraftLocation;
                bUsedActiveAircraft = true;
            }
        }
    }

    const double MinimumAirZ = AnchorZ + 5000.0;

    if (!bUsedActiveAircraft ||
        OutStart.Z < MinimumAirZ)
    {
        OutStart.Z = AnchorZ +
            (bUsedActiveAircraft ? 8000.0 : 15000.0);
    }

    return std::isfinite(OutStart.X) &&
        std::isfinite(OutStart.Y) &&
        std::isfinite(OutStart.Z) &&
        fabs(OutStart.X) < 50000000.0 &&
        fabs(OutStart.Y) < 50000000.0 &&
        fabs(OutStart.Z) < 50000000.0;
}

AFortPlayerPawnAthena* MagnesiumPlayerAISpawner::
    SpawnAirborneForLanding(
        PlayerAIController& AI,
        const FVector& Desired)
{
    FVector Start{};

    if (!PlayerAIBuildSafeAirborneStart(
            AI, Desired, Start))
        return nullptr;

    auto Pawn = SpawnPawnAt(AI, Start, false);

    if (!Pawn)
        return nullptr;

    AI.SetTransitionDamageProtection(true);
    AI.PosVertVel = 0.f;
    AI.bPosGrounded = false;
    AI.GroundedLandingSamples = 0;

    const bool bSkydiving =
        VersionFeatureAdapter::TryBeginSkydiving(Pawn);
    const float AirStartTime =
        VersionFeatureAdapter::GetTimeSeconds();
    AI.bManualAirMovement = !bSkydiving;
    AI.bAirStallSampleValid = false;
    AI.NextAirStallCheckTime = AirStartTime;
    AI.LastManualAirMoveTime = 0.f;
    AI.NextManualAirMoveTime =
        bSkydiving
        ? 0.f
        : AirStartTime +
            0.04f *
            (float)((AI.AIIndex * 13) % 8);

    if (!bSkydiving &&
        Pawn->HasCharacterMovement() &&
        Pawn->CharacterMovement)
    {
        auto Velocity = Pawn->CharacterMovement->Velocity;

        if (Velocity.Z > -400.0)
        {
            Velocity.Z = -1000.0;
            Pawn->CharacterMovement->Velocity = Velocity;
        }
    }

    AI.bAirPawnSeen = true;
    return Pawn;
}

bool MagnesiumPlayerAISpawner::FinishAircraftJumpPawn(PlayerAIController& AI)
{
    auto PC = AI.Entity.PC;
    auto Pawn = AI.GetPawn();

    if (!Pawn)
        return false;

    // Keep the existing pawn and its inventory/cosmetic state. Simulated
    // controllers need their convenience pointer refreshed; native bot
    // controllers have no AFortPlayerControllerAthena.
    if (PC)
        PC->MyFortPawn = Pawn;

    if (AI.Entity.PlayerState)
    {
        Pawn->PlayerState = AI.Entity.PlayerState;
        Pawn->OnRep_PlayerState();
    }

    ReplicationBehavior::SetupPawnReplication(Pawn);

    AI.SetTransitionDamageProtection(true);

    AI.PosVertVel = 0.f;
    AI.bPosGrounded = false;

    FVector Start{};

    if (!PlayerAIBuildSafeAirborneStart(
            AI, AI.LandingTarget, Start))
    {
        return false;
    }

    Pawn->K2_TeleportTo(Start, Pawn->K2_GetActorRotation(), false, true);
    ReplicationBehavior::PushTeleportUpdate(Pawn);

    if (VersionFeatureAdapter::TryBeginSkydiving(Pawn))
    {
        AI.bManualAirMovement = false;
        AI.bAirStallSampleValid = false;
        AI.NextAirStallCheckTime =
            VersionFeatureAdapter::GetTimeSeconds();
        AI.LastManualAirMoveTime = 0.f;
        AI.NextManualAirMoveTime = 0.f;
        AI.bAirPawnSeen = true;
        return true;
    }

    // No observed skydive support: keep the pawn high and protected and let
    // staggered landing ticks resolve terrain. Performing a 25-probe ground
    // search here once per passenger froze full AI lobbies at bus exit.
    if (Pawn->HasCharacterMovement() &&
        Pawn->CharacterMovement)
    {
        auto Velocity = Pawn->CharacterMovement->Velocity;

        if (Velocity.Z > -400.0)
        {
            Velocity.Z = -1000.0;
            Pawn->CharacterMovement->Velocity = Velocity;
        }
    }

    AI.bManualAirMovement = true;
    AI.bAirStallSampleValid = false;
    const float ManualStartTime =
        VersionFeatureAdapter::GetTimeSeconds();
    AI.NextAirStallCheckTime = ManualStartTime;
    AI.LastManualAirMoveTime = 0.f;
    AI.NextManualAirMoveTime =
        ManualStartTime +
        0.04f *
        (float)((AI.AIIndex * 13) % 8);
    AI.bAirPawnSeen = true;
    return true;
}

void MagnesiumPlayerAISpawner::DespawnEntity(
    PlayerAIController& AI,
    const char* Reason,
    bool bSyncPlayersLeft)
{
    auto GameMode = VersionFeatureAdapter::GetGameMode();
    auto PC = AI.Entity.PC;
    auto Inventory = AI.Entity.GetInventory();
    const bool bControllerValid =
        AI.Entity.HasLiveController();
    const bool bPlayerStateValid =
        AI.Entity.HasLivePlayerState();

    AI.SetTransitionDamageProtection(false);

    AIDebugLogger::Verbose("Spawner", "despawning AIPlayer '%s' (%s)", AI.Entity.DisplayName.c_str(), Reason ? Reason : "");

    // ---- Native backend entity ----
    if (AI.Entity.bNativeBacked)
    {
        auto NativePawn = AI.GetPawn();

        if (NativePawn)
            NativePawn->K2_DestroyActor();

        auto EliminatedPawn = AI.EliminatedPawn;
        if (EliminatedPawn != NativePawn &&
            PlayerAIEntity::IsLivePawn(EliminatedPawn))
        {
            EliminatedPawn->K2_DestroyActor();
        }

        auto Bot = AI.Entity.NativeController;

        if (Bot && GameMode && GameMode->HasAliveBots())
        {
            for (int i = 0; i < GameMode->AliveBots.Num(); i++)
            {
                if (GameMode->AliveBots[i] == Bot)
                {
                    GameMode->AliveBots.Remove(i);
                    break;
                }
            }
        }

        if (Inventory)
            Inventory->K2_DestroyActor();

        if (bPlayerStateValid)
            AI.Entity.PlayerState->K2_DestroyActor();

        if (bControllerValid)
            Bot->K2_DestroyActor();

        AI.Entity.NativeController = nullptr;
        AI.Entity.NativePawn = nullptr;
        AI.Entity.PlayerState = nullptr;
        AI.bDeathHandled = true;
        if (bSyncPlayersLeft)
            VersionFeatureAdapter::SyncPlayersLeft(true);
        return;
    }

    auto Pawn = AI.GetPawn();

    if (Pawn)
        Pawn->K2_DestroyActor();

    auto EliminatedPawn = AI.EliminatedPawn;
    if (EliminatedPawn != Pawn &&
        PlayerAIEntity::IsLivePawn(EliminatedPawn))
    {
        EliminatedPawn->K2_DestroyActor();
    }

    if (PC && GameMode)
    {
        for (int i = 0; i < GameMode->AlivePlayers.Num(); i++)
        {
            if (GameMode->AlivePlayers[i] == PC)
            {
                GameMode->AlivePlayers.Remove(i);
                break;
            }
        }
    }

    if (Inventory)
        Inventory->K2_DestroyActor();

    if (bPlayerStateValid)
        AI.Entity.PlayerState->K2_DestroyActor();

    if (bControllerValid)
        PC->K2_DestroyActor();

    AI.Entity.PC = nullptr;
    AI.Entity.PlayerState = nullptr;
    AI.bDeathHandled = true;
    if (bSyncPlayersLeft)
        VersionFeatureAdapter::SyncPlayersLeft(true);
}

// ============================================================================
// MagnesiumPlayerAIFillManager
// ============================================================================

static int PlayerAISpawnFaults = 0;
static float PlayerAISpawnBackoffUntil = 0.f;
static float PlayerAINextFillSpawnTime = 0.f;
static float PlayerAINextFillDiagnosticTime = 0.f;

void MagnesiumPlayerAIFillManager::Reset()
{
    bFillStarted = false;
    FillAllowedAtTime = 0.f;
    PlayerAISpawnFaults = 0;
    PlayerAISpawnBackoffUntil = 0.f;
    PlayerAINextFillSpawnTime = 0.f;
    PlayerAINextFillDiagnosticTime = 0.f;
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

    // Periodic diagnostic so "nothing is spawning" is always explainable
    // from the log.
    if (Current == 0 && Desired > 0 && Now >= PlayerAINextFillDiagnosticTime)
    {
        PlayerAINextFillDiagnosticTime = Now + 10.f;
        AIDebugLogger::Log("Fill", "waiting to fill: desired %d, current %d, faults %d, backoff %.0fs",
            Desired, Current, PlayerAISpawnFaults, PlayerAISpawnBackoffUntil > Now ? PlayerAISpawnBackoffUntil - Now : 0.f);
    }

    if (Current < Desired && Now >= PlayerAISpawnBackoffUntil && Now >= PlayerAINextFillSpawnTime)
    {
        // Gradual fill: one PlayerAI at a time, spaced like players joining.
        PlayerAINextFillSpawnTime = Now + PlayerAIRandRange(PlayerAIConfig::FillSpawnIntervalMin, PlayerAIConfig::FillSpawnIntervalMax);

        PlayerAIController* NewController = nullptr;

        if (!PlayerAITrySpawnOne(&NewController))
        {
            PlayerAISpawnFaults++;
            AIDebugLogger::Error("Fill", "PlayerAI spawn faulted (%d) - contained", PlayerAISpawnFaults);

            if (PlayerAISpawnFaults % 3 == 0)
            {
                // Back off instead of giving up: transient early-lobby
                // faults must not kill the whole fill.
                PlayerAISpawnBackoffUntil = Now + 15.f;
                AIDebugLogger::Error("Fill", "PlayerAI spawning backing off for 15s after repeated faults");
            }
        }
        else if (NewController)
        {
            PlayerAISpawnFaults = 0;
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

void MagnesiumPlayerAIIntegration::ResetLifecycleState()
{
    bConfigLoadedFired = false;
    bServerStartedFired = false;
    bMatchCreatedFired = false;
    bPreMatchFired = false;
    bTransportFired = false;
    bMatchEndedFired = false;
    LastRealPlayerCount = 0;
    LastStatusTime = 0.f;
    SystemFaults = 0;
    bSystemDisabled = false;
    KnownRealPlayerControllers.clear();
    snprintf(StatusLine, sizeof(StatusLine), "PlayerAI: idle");
}

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
    AIDebugLogger::Log(
        "Lifecycle",
        "OnGameserverConfigLoaded (Enable AIs: %s)",
        MagnesiumPlayerAISettings::bEnableAIs.load(
            std::memory_order_relaxed)
            ? "ON"
            : "OFF");
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
    NativePlayerAIBackend::Reset();
}

void MagnesiumPlayerAIIntegration::OnGameserverShutdown()
{
    AIDebugLogger::Log("Lifecycle", "OnGameserverShutdown");
    PlayerAIManager::Shutdown("gameserver shutdown");
    MagnesiumPlayerAIFillManager::Reset();
    NativePlayerAIBackend::Reset();
    VersionFeatureAdapter::ResetCaches();
    ResetLifecycleState();
    bLifecycleTokensSeen = false;
    LifecycleWorldToken = nullptr;
    LifecycleGameStateToken = nullptr;
}

bool MagnesiumPlayerAIIntegration::IsPlayerAIController(void* PlayerController)
{
    return PlayerAIManager::IsPlayerAI((AFortPlayerControllerAthena*)PlayerController);
}

void MagnesiumPlayerAIIntegration::OnAircraftDropZoneEnding()
{
    if (!PlayerAIManager::bInitialized || PlayerAIManager::GetTotalCount() == 0)
        return;

    const float Now = VersionFeatureAdapter::GetTimeSeconds();
    VersionFeatureAdapter::BeginServerTick(Now);
    int Queued = 0;
    int ExitCertified = 0;
    int ExitUnresolved = 0;

    for (auto& AI : PlayerAIManager::GetControllers())
    {
        if (!AI || !AI->Entity.IsValid())
            continue;

        const auto State = AI->GetState();

        if (State == EPlayerAIState::Dead || State == EPlayerAIState::MatchEnded)
            continue;

        auto PC = AI->Entity.PC;

        // The route-end cleanup immediately following this callback rechecks
        // loading state, and a few builds reset transition flags while
        // leaving warmup. Reassert the synthetic acknowledgement before
        // native processing resumes.
        if (PC)
        {
            PlayerAIMarkSyntheticParticipantReady(
                PC,
                (AFortPlayerStateAthena*)PC->PlayerState,
                AI->GetPawn());

            // Every virtual legacy passenger needs the private native
            // "already exited" latch, including bots that jumped earlier.
            // Previously this was only re-applied to entries still reported
            // aboard, so an earlier ready-bit reset left already-gliding bots
            // eligible for the route-end failed-loader kick.
            if (VersionInfo.FortniteVersion < 11.00)
            {
                if (VersionFeatureAdapter::
                        MarkVirtualAircraftExited(PC))
                {
                    ExitCertified++;
                }
                else
                {
                    ExitUnresolved++;
                }
            }
        }

        const bool bAboard =
            PC && VersionFeatureAdapter::IsInAircraft(PC);
        const bool bNeedsJump = !AI->bJumpedFromTransport &&
            (State == EPlayerAIState::InTransport || State == EPlayerAIState::WaitingForTransport ||
             State == EPlayerAIState::ChoosingLandingSpot ||
             State == EPlayerAIState::PreMatchIdle ||
             State == EPlayerAIState::PreMatchWalking ||
             State == EPlayerAIState::PreMatchEmoting);

        if (!bAboard && !bNeedsJump)
            continue;

        // This is intentionally only a cheap state detach.  Calling the
        // native KickFromAircraft path for a connectionless PlayerAI can run
        // death/removal bookkeeping before its replacement skydive pawn is
        // ready.  Clear every sticky aircraft mirror synchronously so the
        // native end-of-route pass cannot see a leftover passenger; the
        // expensive teleport/skydive work remains budgeted below.
        if (PC && (bAboard || bNeedsJump))
            VersionFeatureAdapter::ForceLeaveAircraft(PC);

        // Queue anyone who has not left the bus yet. Performing every
        // kick/teleport/skydive/spawn synchronously in this callback created
        // a second unbounded full-lobby exit path. The normal transport think
        // unboards and transitions at most two queued passengers per tick.
        if (bNeedsJump)
        {
            if (!AI->bHasLandingTarget)
            {
                auto Aircraft = VersionFeatureAdapter::GetAircraft();
                AI->LandingTarget = Aircraft ? Aircraft->K2_GetActorLocation() : FVector{};
                AI->LandingTarget.Z = 0.0;
                AI->bHasLandingTarget = true;
                AI->bLandingTargetGroundValidated = false;
            }

            AI->bForceTransportJump = true;
            AI->bTransportSetupPending = false;
            AI->TransportUnlockedAtTime = Now;
            AI->EarliestJumpTime = Now;
            AI->ForcedJumpTime = Now;

            if (State == EPlayerAIState::PreMatchIdle ||
                State == EPlayerAIState::PreMatchWalking ||
                State == EPlayerAIState::PreMatchEmoting ||
                State == EPlayerAIState::ChoosingLandingSpot)
            {
                AI->SetState(
                    EPlayerAIState::InTransport,
                    "drop zone ending queued");
            }

            Queued++;
        }

    }

    if (Queued || ExitCertified || ExitUnresolved)
        AIDebugLogger::Log(
            "Transport",
            "drop zone ending: certified %d legacy exits, unresolved %d, queued %d budgeted PlayerAI exits",
            ExitCertified, ExitUnresolved, Queued);
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

    auto GameState = VersionFeatureAdapter::GetGameState();
    const bool bWorldTokenChanged =
        bLifecycleTokensSeen &&
        LifecycleWorldToken != World;
    const bool bGameStateTokenChanged =
        bLifecycleTokensSeen && GameState &&
        LifecycleGameStateToken != GameState;
    const bool bLifecycleTokenChanged =
        bWorldTokenChanged || bGameStateTokenChanged;

    if (bLifecycleTokenChanged)
    {
        AIDebugLogger::Log(
            "Lifecycle",
            "world/game-state changed - resetting PlayerAI lifecycle");
        PlayerAIManager::Shutdown("world/game-state changed");
        MagnesiumPlayerAIFillManager::Reset();
        NativePlayerAIBackend::Reset();
        VersionFeatureAdapter::ResetCaches();
        ResetLifecycleState();
    }

    bLifecycleTokensSeen = true;
    LifecycleWorldToken = World;

    if (bWorldTokenChanged || GameState)
        LifecycleGameStateToken = GameState;

    const float Now = VersionFeatureAdapter::GetTimeSeconds();
    VersionFeatureAdapter::BeginServerTick(Now);

    // ---- Lifecycle: config loaded ----
    if (!bConfigLoadedFired && FConfiguration::bReadyToStart)
    {
        bConfigLoadedFired = true;
        OnGameserverConfigLoaded();
    }

    // ---- Lifecycle: gameserver started / joinable ----
    if (MagnesiumPlayerAISettings::bEnableAIs.load(
            std::memory_order_relaxed) &&
        !PlayerAIManager::bInitialized &&
        GUI::gsStatus >= Joinable &&
        Misc::bHookedAll)
    {
        if (!bServerStartedFired)
        {
            bServerStartedFired = true;
            OnGameserverStarted();
        }
    }

    if (!PlayerAIManager::bInitialized)
        return;

    if (MagnesiumPlayerAISettings::bEnableAIs.load(
            std::memory_order_relaxed))
        VersionFeatureAdapter::TickCosmeticCache();

    // Turning this visible toggle off is authoritative in every phase.
    // Continuing to update already-spawned AI after the checkbox disappeared
    // made OFF look ineffective during live matches.
    if (!MagnesiumPlayerAISettings::bEnableAIs.load(
            std::memory_order_relaxed))
    {
        PlayerAIManager::Shutdown("Enable AIs turned off");
        MagnesiumPlayerAIFillManager::Reset();
        NativePlayerAIBackend::Reset();
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

    // Some host flows recycle the same UWorld/GameState for another match.
    // A phase leaving Ended is therefore also a lifecycle token: tear down
    // the completed manager and re-arm every one-shot hook for the next tick.
    if (bMatchEndedFired &&
        Phase != EPlayerAIMatchPhase::Ended)
    {
        AIDebugLogger::Log(
            "Lifecycle",
            "match phase restarted in the same world - rearming PlayerAI");
        PlayerAIManager::Shutdown("same-world match restart");
        MagnesiumPlayerAIFillManager::Reset();
        NativePlayerAIBackend::Reset();
        VersionFeatureAdapter::ResetCaches();
        ResetLifecycleState();
        return;
    }

    // Warm up the native backend (bot mutator + runtime navmesh) while the
    // lobby is still forming so paths exist by the time the AI need them.
    if (Phase == EPlayerAIMatchPhase::PreMatch && NativePlayerAIBackend::IsAvailable())
        NativePlayerAIBackend::EnsureInitialized();

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
    if (bSystemDisabled)
    {
        // A disabled match gets one clean retry after map travel. These
        // token reads are guarded because the disable path is the final
        // containment boundary and must not itself crash the server.
        bool bTokenChanged = false;

        GPlayerAIGuardedNativeCallDepth++;

        __try
        {
            auto World = UWorld::GetWorld();
            auto GameState =
                VersionFeatureAdapter::GetGameState();
            bTokenChanged = bLifecycleTokensSeen &&
                (LifecycleWorldToken != World ||
                 (GameState &&
                  LifecycleGameStateToken != GameState));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            bTokenChanged = false;
        }

        GPlayerAIGuardedNativeCallDepth--;

        if (!bTokenChanged)
            return;
    }

    // Fully disabled and never initialized: Magnesium behaves exactly like
    // it does without the PlayerAI system (zero cost, no guard entered).
    if (!MagnesiumPlayerAISettings::bEnableAIs.load(
            std::memory_order_relaxed) &&
        !PlayerAIManager::bInitialized)
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
