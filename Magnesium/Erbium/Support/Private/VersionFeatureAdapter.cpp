#include "pch.h"
#include "../Public/AIDebugLogger.h"
#include "../Public/AINameGenerator.h"
#include "../Public/AISkillProfile.h"
#include "../Public/FaultGuard.h"
#include "../Public/VersionFeatureAdapter.h"
#include "../../Public/Configuration.h"
#include "../../Public/Finders.h"
#include "../../Public/GUI.h"
#include "../../Public/PlayerLoadout.h"
#include "../../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../../FortniteGame/Public/FortKismetLibrary.h"
#include "../../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../../FortniteGame/Public/FortWeapon.h"
#include <cstdarg>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

static void PlayerAILogInternal(const char* Prefix, const char* Category, const char* Format, va_list Args)
{
    char Buffer[1024];
    vsnprintf(Buffer, sizeof(Buffer), Format, Args);
    printf("[PlayerAI]%s[%s] %s\n", Prefix, Category, Buffer);
}

void AIDebugLogger::Log(const char* Category, const char* Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    PlayerAILogInternal("", Category, Format, Args);
    va_end(Args);
}

void AIDebugLogger::Verbose(const char* Category, const char* Format, ...)
{
    if (!bVerbose)
        return;

    va_list Args;
    va_start(Args, Format);
    PlayerAILogInternal("", Category, Format, Args);
    va_end(Args);
}

void AIDebugLogger::MissingFeature(const char* FeatureName, const char* FallbackDescription)
{
    static std::unordered_set<std::string> Reported;

    if (Reported.count(FeatureName))
        return;

    Reported.insert(FeatureName);
    printf("[PlayerAI][MissingFeature] '%s' is not supported on this version - %s\n", FeatureName,
        FallbackDescription);
}

void AIDebugLogger::Error(const char* Category, const char* Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    PlayerAILogInternal("[Error]", Category, Format, Args);
    va_end(Args);
}

static const char* PlayerAINameParts1[] =
{
    "Shadow", "Frost", "Blaze", "Night", "Storm", "Iron", "Ghost", "Pixel",
    "Turbo", "Cosmic", "Lucky", "Rapid", "Silent", "Crazy", "Mega", "Hyper",
    "Salty", "Sneaky", "Wild", "Neon", "Dark", "Golden", "Crimson", "Icy",
    "Toxic", "Rogue", "Drift", "Nova", "Echo", "Vortex", "Zesty", "Chill",
};

static const char* PlayerAINameParts2[] =
{
    "Wolf", "Hawk", "Falcon", "Sniper", "Raider", "Slayer", "Viper", "Panda",
    "Fox", "Tiger", "Reaper", "Knight", "Ninja", "Wizard", "Pirate", "Racer",
    "Hunter", "Gamer", "Rebel", "Bandit", "Falcons", "Llama", "Shark", "Eagle",
    "Rider", "Striker", "Phantom", "Jumper", "Dasher", "Scout", "Ace", "King",
};

static std::unordered_set<std::string> UsedPlayerAINames;
static uint32_t NextFallbackPlayerAIName = 1;

std::string AINameGenerator::NextName()
{
    for (int Attempt = 0; Attempt < 32; Attempt++)
    {
        std::string Name = PlayerAINameParts1[rand() % (sizeof(PlayerAINameParts1) / sizeof(PlayerAINameParts1[0]))];
        Name += PlayerAINameParts2[rand() % (sizeof(PlayerAINameParts2) / sizeof(PlayerAINameParts2[0]))];

        if (rand() % 2)
            Name += std::to_string(rand() % 100);

        if (!UsedPlayerAINames.count(Name))
        {
            UsedPlayerAINames.insert(Name);
            return Name;
        }
    }

    for (;;)
    {
        std::string Name = "AIPlayer" + std::to_string(NextFallbackPlayerAIName++);

        if (UsedPlayerAINames.insert(Name).second)
            return Name;
    }
}

void AINameGenerator::Reset()
{
    UsedPlayerAINames.clear();
    NextFallbackPlayerAIName = 1;
}

FPlayerAISkillSettings AISkillProfile::GetSettings(EPlayerAISkillProfile Profile)
{
    FPlayerAISkillSettings S{};
    S.Profile = Profile;

    switch (Profile)
    {
    case EPlayerAISkillProfile::Beginner:
        S.AimAccuracy = 0.22f;
        S.ReactionTimeSeconds = 1.6f;
        S.Aggression = 0.20f;
        S.PushChance = 0.15f;
        S.RetreatChance = 0.55f;
        S.ThirdPartyChance = 0.10f;
        S.EngageRange = 9000.f;
        S.DetectionRange = 14000.f;
        S.MovementQuality = 0.35f;
        S.MoveSpeed = 440.f;
        S.LootGreed = 0.75f;
        S.WeaponSwapSkill = 0.30f;
        S.StormAwareness = 0.35f;
        S.HealingDiscipline = 0.30f;
        S.HotDropChance = 0.15f;
        break;

    case EPlayerAISkillProfile::Average:
        S.AimAccuracy = 0.40f;
        S.ReactionTimeSeconds = 1.0f;
        S.Aggression = 0.45f;
        S.PushChance = 0.35f;
        S.RetreatChance = 0.40f;
        S.ThirdPartyChance = 0.20f;
        S.EngageRange = 13000.f;
        S.DetectionRange = 19000.f;
        S.MovementQuality = 0.55f;
        S.MoveSpeed = 520.f;
        S.LootGreed = 0.55f;
        S.WeaponSwapSkill = 0.55f;
        S.StormAwareness = 0.55f;
        S.HealingDiscipline = 0.50f;
        S.HotDropChance = 0.30f;
        break;

    case EPlayerAISkillProfile::Advanced:
        S.AimAccuracy = 0.60f;
        S.ReactionTimeSeconds = 0.55f;
        S.Aggression = 0.60f;
        S.PushChance = 0.55f;
        S.RetreatChance = 0.30f;
        S.ThirdPartyChance = 0.35f;
        S.EngageRange = 17000.f;
        S.DetectionRange = 24000.f;
        S.MovementQuality = 0.85f;
        S.MoveSpeed = 560.f;
        S.LootGreed = 0.40f;
        S.WeaponSwapSkill = 0.85f;
        S.StormAwareness = 0.85f;
        S.HealingDiscipline = 0.80f;
        S.HotDropChance = 0.45f;
        break;

    case EPlayerAISkillProfile::Aggressive:
        S.AimAccuracy = 0.48f;
        S.ReactionTimeSeconds = 0.65f;
        S.Aggression = 0.95f;
        S.PushChance = 0.85f;
        S.RetreatChance = 0.10f;
        S.ThirdPartyChance = 0.60f;
        S.EngageRange = 18000.f;
        S.DetectionRange = 24000.f;
        S.MovementQuality = 0.70f;
        S.MoveSpeed = 560.f;
        S.LootGreed = 0.30f;
        S.WeaponSwapSkill = 0.60f;
        S.StormAwareness = 0.45f;
        S.HealingDiscipline = 0.35f;
        S.HotDropChance = 0.75f;
        break;

    case EPlayerAISkillProfile::Passive:
        S.AimAccuracy = 0.35f;
        S.ReactionTimeSeconds = 1.2f;
        S.Aggression = 0.10f;
        S.PushChance = 0.05f;
        S.RetreatChance = 0.80f;
        S.ThirdPartyChance = 0.05f;
        S.EngageRange = 10000.f;
        S.DetectionRange = 18000.f;
        S.MovementQuality = 0.50f;
        S.MoveSpeed = 500.f;
        S.LootGreed = 0.85f;
        S.WeaponSwapSkill = 0.50f;
        S.StormAwareness = 0.75f;
        S.HealingDiscipline = 0.75f;
        S.HotDropChance = 0.05f;
        break;

    case EPlayerAISkillProfile::Testing:
        S.AimAccuracy = 1.0f;
        S.ReactionTimeSeconds = 0.05f;
        S.Aggression = 1.0f;
        S.PushChance = 1.0f;
        S.RetreatChance = 0.0f;
        S.ThirdPartyChance = 1.0f;
        S.EngageRange = 30000.f;
        S.DetectionRange = 40000.f;
        S.MovementQuality = 1.0f;
        S.MoveSpeed = 600.f;
        S.LootGreed = 0.2f;
        S.WeaponSwapSkill = 1.0f;
        S.StormAwareness = 1.0f;
        S.HealingDiscipline = 1.0f;
        S.HotDropChance = 0.5f;
        break;
    }

    return S;
}

EPlayerAISkillProfile AISkillProfile::PickRandomProfile()
{
    const int Roll = rand() % 100;

    if (Roll < 20) return EPlayerAISkillProfile::Beginner;
    if (Roll < 55) return EPlayerAISkillProfile::Average;
    if (Roll < 70) return EPlayerAISkillProfile::Advanced;
    if (Roll < 85) return EPlayerAISkillProfile::Aggressive;
    return EPlayerAISkillProfile::Passive;
}

const char* AISkillProfile::ToString(EPlayerAISkillProfile Profile)
{
    switch (Profile)
    {
    case EPlayerAISkillProfile::Beginner: return "Beginner";
    case EPlayerAISkillProfile::Average: return "Average";
    case EPlayerAISkillProfile::Advanced: return "Advanced";
    case EPlayerAISkillProfile::Aggressive: return "Aggressive";
    case EPlayerAISkillProfile::Passive: return "Passive";
    case EPlayerAISkillProfile::Testing: return "Testing";
    }
    return "Unknown";
}

extern uint64_t ApplyCharacterCustomization;

static bool PlayerAIGuardedProcessEvent(const UObject* Obj, UFunction* Fn, void* Params)
{
    GGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        Obj->ProcessEvent(Fn, Params);
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GGuardedNativeCallDepth--;
    return bOk;
}

bool VersionFeatureAdapter::SafeCallNoArgs(const UObject* Obj, UFunction* Fn)
{
    if (!Obj || !Fn)
        return false;

    int Size = 0;
    {
        auto Params = Fn->GetParams();
        Size = Params.Size;
    }

    if (Size < 0 || Size > 0x1000)
        Size = 0x1000;

    std::vector<uint8_t> Buffer((size_t)(Size > 0 ? Size : 1), 0);
    return PlayerAIGuardedProcessEvent(Obj, Fn, Buffer.data());
}

AFortGameMode* VersionFeatureAdapter::GetGameMode()
{
    auto World = UWorld::GetWorld();

    if (!World)
        return nullptr;

    return (AFortGameMode*)World->AuthorityGameMode;
}

AFortGameStateAthena* VersionFeatureAdapter::GetGameState()
{
    auto GameMode = GetGameMode();

    if (!GameMode)
        return nullptr;

    return (AFortGameStateAthena*)GameMode->GameState;
}

float VersionFeatureAdapter::GetTimeSeconds()
{
    auto World = UWorld::GetWorld();

    if (!World)
        return 0.f;

    return (float)UGameplayStatics::GetTimeSeconds(World);
}

static float PlayerAILastServerTickTime = -1.f;
static ULONGLONG PlayerAILastBudgetWallTickMs = 0;
static constexpr int PlayerAIGroundTraceBudgetPerTick = 4;
static int PlayerAIGroundTraceBudgetRemaining = PlayerAIGroundTraceBudgetPerTick;
static bool PlayerAIPhaseScanBudgetAvailable = true;
static bool PlayerAIVisualProofBudgetAvailable = true;

void VersionFeatureAdapter::BeginServerTick(float TimeSeconds)
{
    const ULONGLONG WallTickMs = GetTickCount64();
    if (TimeSeconds == PlayerAILastServerTickTime && PlayerAILastBudgetWallTickMs &&
        WallTickMs - PlayerAILastBudgetWallTickMs < 8ULL)
        return;

    PlayerAILastServerTickTime = TimeSeconds;
    PlayerAILastBudgetWallTickMs = WallTickMs;
    PlayerAIGroundTraceBudgetRemaining = PlayerAIGroundTraceBudgetPerTick;
    PlayerAIPhaseScanBudgetAvailable = true;
    PlayerAIVisualProofBudgetAvailable = true;
}

static bool PlayerAIIsLiveSupportObject(const UObject* Object)
{
    if (!Object || !SDK::MemReadable(Object, sizeof(UObject)))
        return false;

    const int32 ObjectIndex = Object->Index;

    if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
        return false;

    auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
    const int32 InvalidObjectFlags = 0x20;

    return Item && Item->GetObject() == Object && !(Item->GetFlags() & InvalidObjectFlags) &&
        Object->Class && SDK::MemReadable(Object->Class, sizeof(UClass));
}

static bool PlayerAISetReflectedReadyBool(UObject* Object, const char* PropertyName, bool Value)
{
    if (!PlayerAIIsLiveSupportObject(Object) || !PropertyName ||
        Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto Property = Object->GetProperty(PropertyName, 0x20000);
    if (!Property)
        return false;

    const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(Offsets::Offset_Internal,
            Offsets::ElementSize)) + sizeof(uint32);
    if (!SDK::MemReadable(Property, RequiredMetadataBytes) || Offsets::FieldMask == 0 ||
        !SDK::MemReadable(Property, static_cast<size_t>(Offsets::FieldMask) + sizeof(uint8)))
    {
        return false;
    }

    const int32 PropertyOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            Property, Offsets::Offset_Internal)));
    const uint32 ElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
    const int32 ArrayDimension = GetFromOffset<int32>(Property,
        Offsets::ElementSize - sizeof(int32));
    const int32 OwnerSize = Object->Class->GetPropertiesSize();
    const uint8 FieldMask = Property->GetFieldMask();

    if (PropertyOffset < 0 || ElementSize != sizeof(uint8) || ArrayDimension != 1 || !FieldMask ||
        OwnerSize <= PropertyOffset)
    {
        return false;
    }

    auto Address = reinterpret_cast<uint8*>(Object) + PropertyOffset;
    if (!SDK::MemReadable(Address, sizeof(uint8)))
        return false;

    Value ? *Address |= FieldMask : *Address &= ~FieldMask;
    return true;
}

static bool PlayerAITryReadReflectedBool(UObject* Object, const char* PropertyName, bool& OutValue)
{
    OutValue = false;
    if (!PlayerAIIsLiveSupportObject(Object) || !PropertyName ||
        Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto Property = Object->GetProperty(PropertyName, 0x20000);
    if (!Property)
        return false;

    const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(Offsets::Offset_Internal,
            Offsets::ElementSize)) + sizeof(uint32);
    if (!SDK::MemReadable(Property, RequiredMetadataBytes) || Offsets::FieldMask == 0 ||
        !SDK::MemReadable(Property, static_cast<size_t>(Offsets::FieldMask) + sizeof(uint8)))
    {
        return false;
    }

    const int32 PropertyOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            Property, Offsets::Offset_Internal)));
    const uint32 ElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
    const int32 ArrayDimension = GetFromOffset<int32>(Property,
        Offsets::ElementSize - sizeof(int32));
    const int32 OwnerSize = Object->Class->GetPropertiesSize();
    const uint8 FieldMask = Property->GetFieldMask();
    if (PropertyOffset < 0 || ElementSize != sizeof(uint8) || ArrayDimension != 1 || !FieldMask ||
        OwnerSize <= PropertyOffset)
    {
        return false;
    }

    auto Address = reinterpret_cast<const uint8*>(Object) + PropertyOffset;
    if (!SDK::MemReadable(Address, sizeof(uint8)))
        return false;

    OutValue = (*Address & FieldMask) != 0;
    return true;
}

void VersionFeatureAdapter::MarkSyntheticParticipantReady(AFortPlayerControllerAthena* PC,
    AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    if (!PC)
        return;

    PlayerAISetReflectedReadyBool(PC, "bHasClientFinishedLoading", true);
    PlayerAISetReflectedReadyBool(PC, "bHasServerFinishedLoading", true);
    PlayerAISetReflectedReadyBool(PC, "bHasFinishedLoading", true);
    PlayerAISetReflectedReadyBool(PC, "bReadyToStartMatch", true);
    PlayerAISetReflectedReadyBool(PC, "bAssignedStartSpawn", true);

    if (Pawn)
    {
        PlayerAISetReflectedReadyBool(PC, "bHasInitiallySpawned", true);
        PlayerAISetReflectedReadyBool(PC, "bClientPawnIsLoaded", true);
    }

    PlayerAISetReflectedReadyBool(PC, "bMarkedAlive", true);

    if (PlayerState)
    {
        PlayerAISetReflectedReadyBool(PlayerState, "bIsSpectator", false);
        PlayerAISetReflectedReadyBool(PlayerState, "bHasFinishedLoading", true);
        PlayerAISetReflectedReadyBool(PlayerState, "bHasStartedPlaying", true);
        MarkReplicatedPropertyDirty(PlayerState, L"bHasStartedPlaying");
        PlayerState->ForceNetUpdate();
    }

    PC->ForceNetUpdate();
}

static bool PlayerAIIsLiveSupportActor(const AActor* Actor)
{
    if (!PlayerAIIsLiveSupportObject(Actor))
        return false;

    return !Actor->HasbActorIsBeingDestroyed() || !Actor->bActorIsBeingDestroyed;
}

bool VersionFeatureAdapter::IsLiveObject(const UObject* Object)
{
    return PlayerAIIsLiveSupportObject(Object);
}

bool VersionFeatureAdapter::IsLiveActor(const AActor* Actor)
{
    return PlayerAIIsLiveSupportActor(Actor);
}

static const void* GSupportWorldToken = nullptr;

void VersionFeatureAdapter::TickServerFrame(const UNetDriver* Driver)
{
    auto World = UWorld::GetWorld();

    if (!World || !Driver || Driver != World->NetDriver)
        return;

    if (GSupportWorldToken != World)
    {
        GSupportWorldToken = World;
        ResetCaches();
    }

    BeginServerTick(GetTimeSeconds());

    TickCosmeticCache();
    RetryPendingPlayersLeftReplication();
}

struct FPlayerAIClassLookupCache
{
    const UClass* Class = nullptr;
    ULONGLONG NextResolveTime = 0;
};

static FPlayerAIClassLookupCache PlayerAIAircraftComponentClassCache;
static FPlayerAIClassLookupCache PlayerAIPawnClassCache;
static FPlayerAIClassLookupCache PlayerAIControllerClassCache;
static FPlayerAIClassLookupCache PlayerAIMovementComponentClassCache;
static FPlayerAIClassLookupCache PlayerAISafeZoneIndicatorClassCache;
static FPlayerAIClassLookupCache PlayerAINetPushModelHelpersClassCache;
static FPlayerAIClassLookupCache PlayerAISkeletalMeshClassCache;
static const UStruct* PlayerAIFortAthenaLoadoutStructCache = nullptr;

static const UClass* PlayerAIResolveCachedClass(
    FPlayerAIClassLookupCache& Cache, const char* ClassName)
{
    if (Cache.Class && PlayerAIIsLiveSupportObject(Cache.Class))
    {
        return Cache.Class;
    }

    if (Cache.Class)
    {
        Cache.Class = nullptr;
        Cache.NextResolveTime = 0;
    }

    const ULONGLONG Now = GetTickCount64();

    if (Now < Cache.NextResolveTime)
        return nullptr;

    Cache.Class = FindClass(ClassName);
    Cache.NextResolveTime = Cache.Class ? 0 : Now + 2000ULL;
    return Cache.Class;
}

static void PlayerAIResetClassLookup(FPlayerAIClassLookupCache& Cache)
{
    Cache.Class = nullptr;
    Cache.NextResolveTime = 0;
}

static UWorld* PlayerAIPhaseLogicWorld = nullptr;
static UFortGameStateComponent_BattleRoyaleGamePhaseLogic* PlayerAIPhaseLogic = nullptr;
static const UClass* PlayerAIPhaseLogicClass = nullptr;
static ULONGLONG PlayerAINextPhaseLogicClassResolveTime = 0;
static ULONGLONG PlayerAINextPhaseLogicResolveTime = 0;
static int PlayerAIPhaseLogicScanCursor = 0;
static int PlayerAIPhaseLogicScanLimit = 0;

static bool PlayerAIIsOwnedByGameState(const UObject* Object, const AFortGameStateAthena* GameState)
{
    if (!PlayerAIIsLiveSupportObject(Object) || !GameState)
        return false;

    auto Outer = Object->Outer;

    for (int Depth = 0; Outer && Depth < 16; Depth++)
    {
        if (Outer == GameState)
            return true;

        if (!PlayerAIIsLiveSupportObject(Outer))
            return false;

        Outer = Outer->Outer;
    }

    return false;
}

// FN 25.20+ moved the authoritative BR phase, aircraft and safe-zone state onto this component.
static UFortGameStateComponent_BattleRoyaleGamePhaseLogic* PlayerAIResolvePhaseLogic()
{
    if (VersionInfo.FortniteVersion < 25.20)
        return nullptr;

    auto World = UWorld::GetWorld();
    auto GameState = VersionFeatureAdapter::GetGameState();

    if (!World || !GameState)
        return nullptr;

    if (PlayerAIPhaseLogicWorld != World)
    {
        PlayerAIPhaseLogicWorld = World;
        PlayerAIPhaseLogic = nullptr;
        PlayerAIPhaseLogicClass = nullptr;
        PlayerAINextPhaseLogicClassResolveTime = 0;
        PlayerAINextPhaseLogicResolveTime = 0;
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = 0;
    }

    if (PlayerAIIsOwnedByGameState(PlayerAIPhaseLogic, GameState))
        return PlayerAIPhaseLogic;

    if (PlayerAIPhaseLogic)
    {
        PlayerAIPhaseLogic = nullptr;
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = 0;
    }

    const ULONGLONG Now = GetTickCount64();

    if (Now < PlayerAINextPhaseLogicResolveTime || !PlayerAIPhaseScanBudgetAvailable)
        return nullptr;

    PlayerAIPhaseScanBudgetAvailable = false;

    if (PlayerAIPhaseLogicClass && !PlayerAIIsLiveSupportObject(PlayerAIPhaseLogicClass))
    {
        PlayerAIPhaseLogicClass = nullptr;
        PlayerAINextPhaseLogicClassResolveTime = 0;
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = 0;
    }

    if (!PlayerAIPhaseLogicClass)
    {
        if (Now < PlayerAINextPhaseLogicClassResolveTime)
            return nullptr;

        PlayerAIPhaseLogicClass = FindClass("FortGameStateComponent_BattleRoyaleGamePhaseLogic");

        if (!PlayerAIPhaseLogicClass)
        {
            PlayerAINextPhaseLogicClassResolveTime = Now + 2000ULL;
            PlayerAINextPhaseLogicResolveTime = Now + 2000ULL;
            return nullptr;
        }

        PlayerAINextPhaseLogicClassResolveTime = 0;
    }

    auto ComponentClass = PlayerAIPhaseLogicClass;

    if (PlayerAIPhaseLogicScanLimit <= 0)
    {
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = TUObjectArray::Num();

        if (PlayerAIPhaseLogicScanLimit <= 0)
        {
            PlayerAINextPhaseLogicResolveTime = Now + 2000ULL;
            return nullptr;
        }
    }

    constexpr int ScanBudget = 512;
    const int End = (std::min)(PlayerAIPhaseLogicScanCursor + ScanBudget,
        PlayerAIPhaseLogicScanLimit);

    for (int i = PlayerAIPhaseLogicScanCursor;
         i < End && i < TUObjectArray::Num(); i++)
    {
        auto Object = TUObjectArray::GetObjectByIndex(i);

        if (!PlayerAIIsLiveSupportObject(Object) || Object->IsDefaultObject() ||
            !Object->IsA(ComponentClass) || !PlayerAIIsOwnedByGameState(Object, GameState))
        {
            continue;
        }

        PlayerAIPhaseLogic = (UFortGameStateComponent_BattleRoyaleGamePhaseLogic*)Object;
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = 0;
        return PlayerAIPhaseLogic;
    }

    PlayerAIPhaseLogicScanCursor = End;

    if (PlayerAIPhaseLogicScanCursor >= PlayerAIPhaseLogicScanLimit)
    {
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = 0;
        PlayerAINextPhaseLogicResolveTime = Now + 2000ULL;
    }

    return nullptr;
}

static UObject* PlayerAIPushModelHelpers = nullptr;
static UFunction* PlayerAIMarkPropertyDirtyFunction = nullptr;
static int32 PlayerAIMarkPropertyDirtyFunctionIndex = -1;
static ULONGLONG PlayerAINextPushModelHelpersResolveTime = 0;
static bool PlayerAIPlayersLeftDirtyPending = false;

static bool PlayerAIValidateMarkPropertyDirty(UFunction* Function)
{
    if (!PlayerAIIsLiveSupportObject(Function))
        return false;

    auto Params = Function->GetParamsNamed();

    if (Params.NameOffsetMap.size() != 2)
        return false;

    if (Params.Size != 0x10)
        return false;

    bool bHasObject = false;
    bool bHasPropertyName = false;

    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "Object")
        {
            bHasObject = Param.Offset == 0 && Param.ElementSize == sizeof(UObject*) &&
                (Param.PropertyFlags & 0x80) != 0;
        }
        else if (Param.Name == "PropertyName")
        {
            bHasPropertyName = Param.Offset == 0x8 && (Param.ElementSize == sizeof(int32) ||
                 Param.ElementSize == sizeof(FName)) && (Param.PropertyFlags & 0x80) != 0;
        }
    }

    return bHasObject && bHasPropertyName;
}

bool VersionFeatureAdapter::MarkReplicatedPropertyDirty(
    const UObject* Object, const wchar_t* PropertyName)
{
    if (VersionInfo.FortniteVersion < 19.0 || !PlayerAIIsLiveSupportObject(Object) || !PropertyName)
    {
        return false;
    }

    if (!PlayerAIIsLiveSupportObject(PlayerAIPushModelHelpers) || !PlayerAIIsLiveSupportObject(
            PlayerAIMarkPropertyDirtyFunction) || PlayerAIMarkPropertyDirtyFunction->Index !=
            PlayerAIMarkPropertyDirtyFunctionIndex)
    {
        PlayerAIPushModelHelpers = nullptr;
        PlayerAIMarkPropertyDirtyFunction = nullptr;
        PlayerAIMarkPropertyDirtyFunctionIndex = -1;

        const ULONGLONG Now = GetTickCount64();

        if (Now < PlayerAINextPushModelHelpersResolveTime)
            return false;

        PlayerAINextPushModelHelpersResolveTime = Now + 2000ULL;

        auto HelpersClass = Offsets::StaticFindObject ? (UClass*)SDK::StaticFindObject(
                L"/Script/Engine.NetPushModelHelpers", UClass::StaticClass()) : nullptr;

        if (!HelpersClass)
            HelpersClass = const_cast<UClass*>(PlayerAIResolveCachedClass(
                        PlayerAINetPushModelHelpersClassCache, "NetPushModelHelpers"));

        PlayerAIPushModelHelpers = HelpersClass ? HelpersClass->GetDefaultObj() : nullptr;
        PlayerAIMarkPropertyDirtyFunction = PlayerAIPushModelHelpers
            ? PlayerAIPushModelHelpers->GetFunction("MarkPropertyDirty") : nullptr;

        if (!PlayerAIIsLiveSupportObject(PlayerAIPushModelHelpers) ||
            !PlayerAIValidateMarkPropertyDirty(PlayerAIMarkPropertyDirtyFunction))
        {
            PlayerAIPushModelHelpers = nullptr;
            PlayerAIMarkPropertyDirtyFunction = nullptr;
            PlayerAIMarkPropertyDirtyFunctionIndex = -1;
            return false;
        }

        PlayerAIMarkPropertyDirtyFunctionIndex = PlayerAIMarkPropertyDirtyFunction->Index;
        PlayerAINextPushModelHelpersResolveTime = 0;
    }

    bool bSucceeded = false;
    GGuardedNativeCallDepth++;

    __try
    {
        alignas(16) uint8 Params[0x20]{};
        UObject* MutableObject = (UObject*)Object;
        FName ReplicatedPropertyName(PropertyName);
        memcpy(Params, &MutableObject, sizeof(MutableObject));
        memcpy(Params + 0x8, &ReplicatedPropertyName, VersionInfo.FortniteVersion >= 20.0
                ? sizeof(int32) : sizeof(FName));
        PlayerAIPushModelHelpers->ProcessEvent(PlayerAIMarkPropertyDirtyFunction, Params);
        bSucceeded = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        PlayerAIPushModelHelpers = nullptr;
        PlayerAIMarkPropertyDirtyFunction = nullptr;
        PlayerAIMarkPropertyDirtyFunctionIndex = -1;
        PlayerAINextPushModelHelpersResolveTime = GetTickCount64() + 2000ULL;
    }

    GGuardedNativeCallDepth--;
    return bSucceeded;
}

static VersionFeatureAdapter::FIsManagedAIControllerFn
    GIsManagedAIController = nullptr;
static VersionFeatureAdapter::FHasManagedAIControllersFn
    GHasManagedAIControllers = nullptr;

void VersionFeatureAdapter::SetManagedAIControllerHooks(FIsManagedAIControllerFn IsManaged,
    FHasManagedAIControllersFn HasAny)
{
    GIsManagedAIController = IsManaged;
    GHasManagedAIControllers = HasAny;
}

bool VersionFeatureAdapter::IsManagedAIController(const AFortPlayerControllerAthena* PC)
{
    return PC && GIsManagedAIController && GIsManagedAIController(PC);
}

bool VersionFeatureAdapter::HasManagedAIControllers()
{
    return GHasManagedAIControllers && GHasManagedAIControllers();
}

int VersionFeatureAdapter::CountAliveParticipants()
{
    auto GameMode = GetGameMode();

    if (!GameMode)
        return 0;

    std::unordered_set<const AActor*> UniqueControllers;

    for (auto Controller : GameMode->AlivePlayers)
        if (Controller)
            UniqueControllers.insert(Controller);

    if (GameMode->HasAliveBots())
    {
        for (auto Controller : GameMode->AliveBots)
            if (Controller)
                UniqueControllers.insert(Controller);
    }

    return (int)UniqueControllers.size();
}

void VersionFeatureAdapter::ReplicatePlayersLeft(
    AFortGameStateAthena* GameState, int PlayersLeft, bool bForce)
{
    if (!GameState)
        return;

    if (PlayersLeft < 0)
        PlayersLeft = 0;
    if (PlayersLeft > 255)
        PlayersLeft = 255;

    const bool bChanged = GameState->PlayersLeft != PlayersLeft;

    if (!bChanged && !bForce && !PlayerAIPlayersLeftDirtyPending)
        return;

    GameState->PlayersLeft = PlayersLeft;

    if (VersionInfo.FortniteVersion >= 19.0)
    {
        PlayerAIPlayersLeftDirtyPending = !MarkReplicatedPropertyDirty(GameState, L"PlayersLeft");
    }
    else
    {
        PlayerAIPlayersLeftDirtyPending = false;
    }

    GameState->OnRep_PlayersLeft();
    GameState->FlushNetDormancy();
    GameState->ForceNetUpdate();
}

void VersionFeatureAdapter::SyncPlayersLeft(bool bForce)
{
    auto GameState = GetGameState();

    if (GameState)
        ReplicatePlayersLeft(GameState, CountAliveParticipants(), bForce);
}

void VersionFeatureAdapter::RetryPendingPlayersLeftReplication()
{
    if (!PlayerAIPlayersLeftDirtyPending)
        return;

    auto GameState = GetGameState();

    if (GameState)
        ReplicatePlayersLeft(GameState, GameState->PlayersLeft, true);
}

int VersionFeatureAdapter::GetMaxPlayerCount()
{
    auto GameMode = GetGameMode();

    if (GameMode && GameMode->HasGameSession() && GameMode->GameSession &&
        GameMode->GameSession->HasMaxPlayers())
    {
        int Max = GameMode->GameSession->MaxPlayers;

        if (Max > 0 && Max <= 255)
            return Max;
    }

    // Fallback: playlist max players.
    auto GameState = GetGameState();
    const auto Playlist = AFortGameMode::GetActivePlaylist(GameState);

    if (Playlist && Playlist->HasMaxPlayers() && Playlist->MaxPlayers > 0 && Playlist->MaxPlayers <= 255)
        return Playlist->MaxPlayers;

    AIDebugLogger::MissingFeature("MaxPlayerCount", "using default of 100");
    return 100;
}

EPlayerAIMatchPhase VersionFeatureAdapter::GetMatchPhase()
{
    if (GUI::gsStatus == Ended)
        return EPlayerAIMatchPhase::Ended;

    if (GUI::gsStatus == NotReady)
        return EPlayerAIMatchPhase::WaitingForServer;

    auto GameState = GetGameState();

    if (!GameState)
        return EPlayerAIMatchPhase::WaitingForServer;

    // Component-driven seasons (27.11, 30.00) still carry a stale GameState phase field.
    if (auto PhaseLogic = PlayerAIResolvePhaseLogic())
    {
        const auto GamePhaseOffset = PhaseLogic->GetOffset("GamePhase");

        if (GamePhaseOffset != (uint32)-1 && GamePhaseOffset < 0x10000)
        {
            const uint8 Phase = GetFromOffset<uint8>(PhaseLogic, GamePhaseOffset);

            if (Phase >= 5)
                return EPlayerAIMatchPhase::Ended;
            if (Phase == 4)
                return EPlayerAIMatchPhase::InProgress;
            if (Phase == 3)
                return EPlayerAIMatchPhase::Transport;
            if (Phase == 2 || GUI::gsStatus == Joinable)
                return EPlayerAIMatchPhase::PreMatch;
        }
    }

    if (GameState->HasGamePhase())
    {
        const uint8 Phase = GameState->GamePhase;

        if (Phase >= 5)
            return EPlayerAIMatchPhase::Ended;
        if (Phase == 4)
            return EPlayerAIMatchPhase::InProgress;
        if (Phase == 3)
            return EPlayerAIMatchPhase::Transport;
        if (GUI::gsStatus == Joinable)
            return EPlayerAIMatchPhase::PreMatch;
        return EPlayerAIMatchPhase::InProgress;
    }

    // Fallback without a GamePhase property.
    if (GUI::gsStatus == Joinable)
        return EPlayerAIMatchPhase::PreMatch;
    if (GUI::gsStatus == StartedMatch)
        return EPlayerAIMatchPhase::InProgress;

    return EPlayerAIMatchPhase::WaitingForServer;
}

static bool bEmoteCacheBuilt = false;
static std::vector<UObject*> CachedEmoteAssets;

static void BuildEmoteCache()
{
    if (bEmoteCacheBuilt)
        return;

    bEmoteCacheBuilt = true;

    const wchar_t* EmotePaths[] =
    {
        L"/Game/Athena/Items/Cosmetics/Dances/EID_DanceMoves.EID_DanceMoves",
        L"/Game/Athena/Items/Cosmetics/Dances/EID_Floss.EID_Floss",
        L"/Game/Athena/Items/Cosmetics/Dances/EID_Fresh.EID_Fresh",
        L"/Game/Athena/Items/Cosmetics/Dances/EID_RideThePony_Athena.EID_RideThePony_Athena",
        L"/Game/Athena/Items/Cosmetics/Dances/EID_Robot.EID_Robot",
        L"/Game/Athena/Items/Cosmetics/Dances/EID_Worm.EID_Worm",
        L"/Game/Athena/Items/Cosmetics/Dances/EID_Accolades.EID_Accolades",
    };

    for (auto Path : EmotePaths)
    {
        auto Asset = Offsets::StaticFindObject ? (const UAthenaDanceItemDefinition*)
                SDK::StaticFindObject(Path, UAthenaDanceItemDefinition::StaticClass()) : nullptr;

        if (Asset)
            CachedEmoteAssets.push_back((UObject*)Asset);
    }

    if (CachedEmoteAssets.empty())
        AIDebugLogger::MissingFeature("Emotes", "PlayerAI players keep walking/idling/jumping instead");
    else
        AIDebugLogger::Log("Emotes", "%d emote assets available on this version",
            (int)CachedEmoteAssets.size());
}

bool VersionFeatureAdapter::SupportsEmotes()
{
    BuildEmoteCache();
    return !CachedEmoteAssets.empty();
}

UObject* VersionFeatureAdapter::GetRandomEmoteAsset()
{
    BuildEmoteCache();

    if (CachedEmoteAssets.empty())
        return nullptr;

    return CachedEmoteAssets[rand() % CachedEmoteAssets.size()];
}

void VersionFeatureAdapter::PlayEmote(AFortPlayerControllerAthena* PC, UObject* EmoteAsset)
{
    if (!PC || !EmoteAsset)
        return;

    AFortPlayerControllerAthena::PlayEmoteInternal(PC, EmoteAsset);
}

bool VersionFeatureAdapter::SupportsThankDriver(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return false;

    static int Supported = -1;

    if (Supported == -1)
    {
        Supported = PC->GetFunction("ServerThankBusDriver") != nullptr ? 1 : 0;

        if (!Supported)
            AIDebugLogger::MissingFeature("ThankBusDriver", "PlayerAI players skip thanking the driver");
    }

    return Supported == 1;
}

bool VersionFeatureAdapter::ThankDriver(AFortPlayerControllerAthena* PC)
{
    if (!SupportsThankDriver(PC))
        return false;

    auto Fn = PC->GetFunction("ServerThankBusDriver");

    if (!Fn)
        return false;

    static bool bThankDisabled = false;

    if (bThankDisabled)
        return false;

    if (!SafeCallNoArgs(PC, Fn))
    {
        bThankDisabled = true;
        AIDebugLogger::MissingFeature("ThankBusDriverForPlayerAI", "thank-driver faulted and was disabled");
        return false;
    }

    return true;
}

AFortAthenaAircraft* VersionFeatureAdapter::GetAircraft()
{
    auto ValidateAircraft = [](AFortAthenaAircraft* Aircraft)-> AFortAthenaAircraft*
        {
            auto AircraftClass = AFortAthenaAircraft::StaticClass();

            return AircraftClass && PlayerAIIsLiveSupportActor(Aircraft) &&
                Aircraft->IsA(AircraftClass) ? Aircraft : nullptr;
        };

    if (auto PhaseLogic = PlayerAIResolvePhaseLogic())
    {
        if (PhaseLogic->HasAircrafts_GameState() && PhaseLogic->Aircrafts_GameState.Num() > 0)
        {
            if (auto Aircraft = ValidateAircraft(PhaseLogic->Aircrafts_GameState[0].Get()))
            {
                return Aircraft;
            }
        }

        if (PhaseLogic->HasAircrafts_GameMode() && PhaseLogic->Aircrafts_GameMode.Num() > 0)
        {
            if (auto Aircraft = ValidateAircraft(PhaseLogic->Aircrafts_GameMode[0].Get()))
            {
                return Aircraft;
            }
        }
    }

    auto GameState = GetGameState();

    if (!GameState)
        return nullptr;

    if (GameState->HasAircrafts())
        return GameState->Aircrafts.Num() > 0 ? ValidateAircraft(GameState->Aircrafts[0]) : nullptr;

    if (GameState->HasAircraft())
        return ValidateAircraft(GameState->Aircraft);

    return nullptr;
}

EPlayerAIAircraftDropState
VersionFeatureAdapter::GetAircraftDropState(float TimeSeconds)
{
    bool bLocked = false;
    bool bOpen = false;

    auto ReadPhaseStep = [&bLocked, &bOpen](const UObject* Owner)
        {
            if (!Owner)
                return;

            const auto Offset = Owner->GetOffset("GamePhaseStep");

            if (Offset == (uint32)-1 || Offset >= 0x10000 || !SDK::MemReadable(
                    (const uint8*)Owner + Offset, sizeof(uint8)))
            {
                return;
            }

            const uint8 Step = GetFromOffset<uint8>(Owner, Offset);

            if (Step == (uint8)EAthenaGamePhaseStep::BusLocked)
                bLocked = true;
            else if (Step >= (uint8)EAthenaGamePhaseStep::BusFlying)
                bOpen = true;
        };

    // Some versions publish bAircraftIsLocked as false while their phase step still says BusLocked.
    auto PhaseLogic = PlayerAIResolvePhaseLogic();

    if (PhaseLogic)
    {
        ReadPhaseStep(PhaseLogic);

        if (PhaseLogic->HasbAircraftIsLocked() && PhaseLogic->bAircraftIsLocked)
        {
            bLocked = true;
        }
    }
    else
    {
        // Legacy builds (including 10.40) keep GamePhaseStep on GameState.
        ReadPhaseStep(GetGameState());
    }

    if (auto Aircraft = GetAircraft())
    {
        bool bHaveStartSignal = false;
        float DropStart = 0.f;

        if (Aircraft->HasDropStartTime() && std::isfinite((double)Aircraft->DropStartTime) &&
            Aircraft->DropStartTime > 1.f)
        {
            DropStart = Aircraft->DropStartTime;
            bHaveStartSignal = true;
        }
        else if (Aircraft->HasFlightStartTime() && Aircraft->HasTimeTillDropStart() &&
            std::isfinite((double)Aircraft->FlightStartTime) &&
            std::isfinite((double)Aircraft->TimeTillDropStart) && Aircraft->FlightStartTime > 0.f &&
            Aircraft->TimeTillDropStart >= 0.f)
        {
            DropStart = Aircraft->FlightStartTime + Aircraft->TimeTillDropStart;
            bHaveStartSignal = true;
        }
        else if (Aircraft->HasFlightElapsedTime() && Aircraft->HasTimeTillDropStart() &&
            std::isfinite((double)Aircraft->FlightElapsedTime) &&
            std::isfinite((double)Aircraft->TimeTillDropStart) &&
            Aircraft->TimeTillDropStart >= 0.f)
        {
            bHaveStartSignal = true;

            if (Aircraft->FlightElapsedTime + 0.01f <Aircraft->TimeTillDropStart)
            {
                bLocked = true;
            }
            else
            {
                bOpen = true;
            }
        }

        if (bHaveStartSignal && DropStart > 0.f)
        {
            if (TimeSeconds + 0.01f < DropStart)
                bLocked = true;
            else
                bOpen = true;
        }
    }

    if (bLocked)
        return EPlayerAIAircraftDropState::Locked;
    if (bOpen)
        return EPlayerAIAircraftDropState::Open;
    return EPlayerAIAircraftDropState::Unknown;
}

static bool PlayerAITryReadInAircraftBit(const UObject* Object, bool& OutInAircraft)
{
    if (!Object)
        return false;

    auto Property = Object->GetProperty("bInAircraft", 0x20000);

    if (!Property)
        return false;

    const auto Offset = GetFromOffset<uint32>(Property, Offsets::Offset_Internal);
    const auto Mask = Property->GetFieldMask();

    if (!Mask || Offset >= 0x20000)
        return false;

    OutInAircraft = (GetFromOffset<uint8>(Object, Offset) & Mask) != 0;
    return true;
}

static bool PlayerAITryQueryInAircraft(AFortPlayerControllerAthena* PC,
    UFunction* Function, bool& OutInAircraft)
{
    if (!PC || !Function)
        return false;

    bool bCalled = false;
    GGuardedNativeCallDepth++;

    __try
    {
        OutInAircraft = PC->Call<bool>(Function);
        bCalled = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    GGuardedNativeCallDepth--;
    return bCalled;
}

bool VersionFeatureAdapter::IsInAircraft(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return false;

    if (auto Function = PC->GetFunction("IsInAircraft"))
    {
        bool bInAircraft = false;

        if (PlayerAITryQueryInAircraft(PC, Function, bInAircraft) && bInAircraft)
        {
            return true;
        }
    }

    bool bInAircraft = false;

    if (PlayerAITryReadInAircraftBit(PC, bInAircraft) && bInAircraft)
    {
        return true;
    }

    if (PC->PlayerState && PlayerAITryReadInAircraftBit(PC->PlayerState, bInAircraft) &&
        bInAircraft)
    {
        return true;
    }

    auto AircraftComponentClass = PlayerAIResolveCachedClass(PlayerAIAircraftComponentClassCache,
            "FortControllerComponent_Aircraft");

    if (AircraftComponentClass)
    {
        auto Component = PC->GetComponentByClass(AircraftComponentClass);

        if (PlayerAITryReadInAircraftBit(Component, bInAircraft) && bInAircraft)
        {
            return true;
        }
    }

    return false;
}

bool VersionFeatureAdapter::EnterAircraft(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return false;

    auto Aircraft = GetAircraft();

    if (!Aircraft)
        return false;

    if (!AFortPlayerControllerAthena::EnterAircraftOG)
    {
        AIDebugLogger::MissingFeature("EnterAircraft", "PlayerAI uses landing teleport fallback");
        return false;
    }

    auto CompClass = PlayerAIResolveCachedClass(PlayerAIAircraftComponentClassCache,
            "FortControllerComponent_Aircraft");

    if (CompClass)
    {
        auto Component = PC->GetComponentByClass(CompClass);

        if (!Component)
            return false;

        AFortPlayerControllerAthena::EnterAircraftOG(Component, Aircraft);
    }
    else
    {
        AFortPlayerControllerAthena::EnterAircraftOG(PC, Aircraft);
    }

    return IsInAircraft(PC);
}

bool VersionFeatureAdapter::JumpFromAircraft(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return false;

    if (IsInAircraft(PC))
    {
        auto CompClass = PlayerAIResolveCachedClass(PlayerAIAircraftComponentClassCache,
                "FortControllerComponent_Aircraft");

        const UObject* Target = PC;

        if (CompClass)
        {
            auto Component = PC->GetComponentByClass(CompClass);

            if (Component)
                Target = Component;
        }

        auto Fn = Target->GetFunction("ServerAttemptAircraftJump");

        if (Fn)
            SafeCallNoArgs(Target, Fn);

        if (!IsInAircraft(PC) && PC->MyFortPawn)
            return true;
    }

    ForceLeaveAircraft(PC);
    return PC->Pawn != nullptr || PC->MyFortPawn != nullptr;
}

static bool PlayerAITryClearInAircraftBit(const UObject* Obj)
{
    if (!Obj)
        return false;

    auto Prop = Obj->GetProperty("bInAircraft", 0x20000);

    if (!Prop)
        return false;

    const auto Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
    const auto Mask = Prop->GetFieldMask();

    if (!Mask || Offset >= 0x20000)
        return false;

    GetFromOffset<uint8>(Obj, Offset) &= ~Mask;
    return true;
}

struct FPlayerAILegacyAircraftExitLatchCache
{
    bool bResolved = false;
    bool bSupported = false;
    uint32 ExitLatchOffset = 0;
    uint32 ReadyOffset = 0;
    uint8 ReadyMask = 0;
};

static FPlayerAILegacyAircraftExitLatchCache
    PlayerAILegacyAircraftExitLatchCache;

struct FPlayerAICmpByteZeroInstruction
{
    uint8 BaseRegister = 0;
    uint32 Displacement = 0;
    size_t Size = 0;
};

static bool PlayerAITryDecodeCmpByteZero(const uint8* Code, size_t Remaining,
    FPlayerAICmpByteZeroInstruction& Out)
{
    if (!Code || Remaining < 7)
        return false;

    size_t Cursor = 0;
    uint8 Rex = 0;

    if ((Code[Cursor] & 0xF0) == 0x40)
    {
        Rex = Code[Cursor++];

        if (Remaining - Cursor < 7)
            return false;
    }

    if (Code[Cursor++] != 0x80)
        return false;

    const uint8 ModRM = Code[Cursor++];

    // mod=10 (disp32), /7 (CMP).
    if ((ModRM & 0xF8) != 0xB8)
        return false;

    uint8 BaseRegister = ModRM & 7;

    if (BaseRegister == 4)
    {
        if (Remaining <= Cursor)
            return false;

        const uint8 Sib = Code[Cursor++];
        const uint8 IndexRegister = ((Sib >> 3) & 7) + ((Rex & 0x02) ? 8 : 0);

        if (IndexRegister != 4)
            return false;

        BaseRegister = (Sib & 7) + ((Rex & 0x01) ? 8 : 0);
    }
    else
    {
        BaseRegister += (Rex & 0x01) ? 8 : 0;
    }

    if (Remaining - Cursor < sizeof(uint32) + 1)
        return false;

    uint32 Displacement = 0;
    memcpy(&Displacement, Code + Cursor, sizeof(Displacement));
    Cursor += sizeof(Displacement);

    if (Code[Cursor++] != 0)
        return false;

    Out.BaseRegister = BaseRegister;
    Out.Displacement = Displacement;
    Out.Size = Cursor;
    return true;
}

static bool PlayerAIFunctionSavesThisInRegister(const uint8* Code, size_t End,
    uint8 ExpectedRegister)
{
    if (!Code || ExpectedRegister > 15)
        return false;

    for (size_t At = 0; At + 2 < End; At++)
    {
        size_t Cursor = At;
        uint8 Rex = 0;

        if ((Code[Cursor] & 0xF0) == 0x40)
            Rex = Code[Cursor++];

        if (Cursor + 1 >= End)
            break;

        const uint8 Opcode = Code[Cursor++];

        if (Opcode != 0x8B && Opcode != 0x89)
            continue;

        const uint8 ModRM = Code[Cursor];

        if ((ModRM & 0xC0) != 0xC0)
            continue;

        const uint8 Reg = ((ModRM >> 3) & 7) + ((Rex & 0x04) ? 8 : 0);
        const uint8 Rm = (ModRM & 7) + ((Rex & 0x01) ? 8 : 0);
        const uint8 Destination = Opcode == 0x8B ? Reg : Rm;
        const uint8 Source = Opcode == 0x8B ? Rm : Reg;

        if (Source == 1 && Destination == ExpectedRegister)
        {
            return true;
        }
    }

    return false;
}

static bool PlayerAIResolveLegacyAircraftExitLatch(AFortPlayerControllerAthena* PC)
{
    auto& Cache = PlayerAILegacyAircraftExitLatchCache;

    if (Cache.bResolved)
        return Cache.bSupported;

    Cache.bResolved = true;

    if (!PC)
        return false;

    auto ReadyProperty = PC->GetProperty("bReadyToStartMatch", 0x20000);

    if (!ReadyProperty)
    {
        AIDebugLogger::Error("Transport",
            "legacy aircraft exit latch unresolved: bReadyToStartMatch was not reflected");
        return false;
    }

    const uint32 ReadyOffset = GetFromOffset<uint32>(ReadyProperty, Offsets::Offset_Internal);
    const uint8 ReadyMask = ReadyProperty->GetFieldMask();

    if (!ReadyMask || ReadyOffset >= 0x20000)
    {
        AIDebugLogger::Error("Transport",
            "legacy aircraft exit latch unresolved: invalid ready field metadata");
        return false;
    }

    const uint64 EnterAircraft = FindEnterAircraft();
    constexpr size_t ScanSize = 0x600;
    const uint64 WarningReference = Memcury::Scanner::FindStringRef(
            L"EnterAircraft: [%s] is attempting to enter aircraft after having already exited.",
            true, 0, VersionInfo.FortniteVersion >= 19).Get();

    if (!EnterAircraft || WarningReference < EnterAircraft ||
        WarningReference >= EnterAircraft + ScanSize || !SDK::MemReadable(
            (const void*)EnterAircraft, ScanSize))
    {
        AIDebugLogger::Error("Transport",
            "legacy aircraft exit latch unresolved: EnterAircraft warning/code validation failed");
        return false;
    }

    const auto Code = reinterpret_cast<const uint8*>(EnterAircraft);
    const size_t WarningAt = (size_t)(WarningReference - EnterAircraft);
    uint32 ResolvedOffset = 0;
    int MatchCount = 0;

    for (size_t FirstAt = 0;
         FirstAt + 7 <= ScanSize;
         FirstAt++)
    {
        FPlayerAICmpByteZeroInstruction First{};

        if (!PlayerAITryDecodeCmpByteZero(Code + FirstAt, ScanSize - FirstAt, First))
        {
            continue;
        }

        // The exit latch sits in the same bool cluster just before the reflected ready-to-start field.
        if (First.Displacement == 0 || First.Displacement >= ReadyOffset ||
            ReadyOffset - First.Displacement > 0x40 || FirstAt >= WarningAt ||
            WarningAt - FirstAt > 0x100 || !PlayerAIFunctionSavesThisInRegister(
                Code, FirstAt, First.BaseRegister))
        {
            continue;
        }

        const size_t DesiredPairEnd = FirstAt + 0x300;
        const size_t PairEnd = DesiredPairEnd < ScanSize ? DesiredPairEnd : ScanSize;
        bool bFoundPair = false;

        for (size_t SecondAt = FirstAt + First.Size;
             SecondAt + 7 <= PairEnd;
             SecondAt++)
        {
            FPlayerAICmpByteZeroInstruction Second{};

            if (!PlayerAITryDecodeCmpByteZero(Code + SecondAt, ScanSize - SecondAt, Second))
            {
                continue;
            }

            if (Second.BaseRegister == First.BaseRegister && Second.Displacement + 1 ==
                    First.Displacement)
            {
                bFoundPair = true;
                break;
            }
        }

        if (!bFoundPair)
            continue;

        ResolvedOffset = First.Displacement;
        MatchCount++;
    }

    if (MatchCount != 1 || ResolvedOffset == 0)
    {
        AIDebugLogger::Error("Transport",
            "legacy aircraft exit latch unresolved: expected one validated compare pair, found %d",
            MatchCount);
        return false;
    }

    Cache.ExitLatchOffset = ResolvedOffset;
    Cache.ReadyOffset = ReadyOffset;
    Cache.ReadyMask = ReadyMask;
    Cache.bSupported = true;

    AIDebugLogger::Log("Transport",
        "resolved legacy aircraft exit latch relative to bReadyToStartMatch (%u bytes before)",
        ReadyOffset - ResolvedOffset);
    return true;
}

static bool PlayerAITryApplyLegacyAircraftExitLatch(AFortPlayerControllerAthena* PC)
{
    if (!PC || VersionInfo.FortniteVersion >= 11.00 ||
        !VersionFeatureAdapter::IsManagedAIController(PC) ||
        !PlayerAIResolveLegacyAircraftExitLatch(PC))
    {
        return false;
    }

    const auto& Cache = PlayerAILegacyAircraftExitLatchCache;
    auto ReadyAddress = reinterpret_cast<const uint8*>(PC) + Cache.ReadyOffset;
    auto ExitStateAddress = reinterpret_cast<uint8*>(PC) + Cache.ExitLatchOffset - 1;

    if (!SDK::MemReadable(ReadyAddress, 1) || !SDK::MemReadable(ExitStateAddress, sizeof(uint16)) ||
        (*ReadyAddress & Cache.ReadyMask) == 0)
    {
        return false;
    }

    uint16 CurrentState = 0;
    memcpy(&CurrentState, ExitStateAddress, sizeof(CurrentState));

    if (CurrentState == 0x0100)
        return true;

    const uint8 InAircraftByte = static_cast<uint8>(CurrentState & 0xFF);
    const uint8 HasExitedByte = static_cast<uint8>(CurrentState >> 8);

    // Both native fields are byte booleans, so anything above 1 means the inferred layout is wrong.
    if (InAircraftByte > 1 || HasExitedByte > 1)
    {
        return false;
    }

    const uint16 ExitedState = 0x0100;
    memcpy(ExitStateAddress, &ExitedState, sizeof(ExitedState));
    return true;
}

static UFortControllerComponent_Aircraft* PlayerAITryGetAircraftComponent(
    AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return nullptr;

    auto ComponentClass = PlayerAIResolveCachedClass(PlayerAIAircraftComponentClassCache,
            "FortControllerComponent_Aircraft");

    if (!ComponentClass)
        return nullptr;

    UFortControllerComponent_Aircraft* Result = nullptr;
    auto Function = PC->GetFunction("GetAircraftComponent");

    if (Function)
    {
        GGuardedNativeCallDepth++;

        __try
        {
            Result = PC->Call<UFortControllerComponent_Aircraft*>(Function);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Result = nullptr;
        }

        GGuardedNativeCallDepth--;
    }

    if ((!PlayerAIIsLiveSupportObject(Result) || !Result->IsA(ComponentClass)))
    {
        Result = (UFortControllerComponent_Aircraft*)
            PC->GetComponentByClass(ComponentClass);
    }

    return PlayerAIIsLiveSupportObject(Result) && Result->IsA(ComponentClass) ? Result : nullptr;
}

bool VersionFeatureAdapter::MarkVirtualAircraftExited(AFortPlayerControllerAthena* PC)
{
    if (VersionInfo.FortniteVersion >= 11.00)
        return false;

    auto ControllerClass = PlayerAIResolveCachedClass(PlayerAIControllerClassCache,
            "FortPlayerControllerAthena");

    if (!ControllerClass || !PlayerAIIsLiveSupportActor(PC) || !PC->IsA(ControllerClass) ||
        !VersionFeatureAdapter::IsManagedAIController(PC))
    {
        return false;
    }

    return PlayerAITryApplyLegacyAircraftExitLatch(PC);
}

static AFortPlayerPawnAthena* PlayerAIRefreshControllerPawn(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return nullptr;

    auto PawnClass = PlayerAIResolveCachedClass(PlayerAIPawnClassCache, "FortPlayerPawnAthena");
    auto IsLiveAthenaPawn = [PawnClass](AFortPlayerPawnAthena* Candidate)
        {
            return PawnClass && PlayerAIIsLiveSupportActor(Candidate) && Candidate->IsA(PawnClass);
        };
    AFortPlayerPawnAthena* Pawn = nullptr;

    if (IsLiveAthenaPawn(PC->Pawn) && PC->Pawn->Controller == PC)
        Pawn = PC->Pawn;
    else if (IsLiveAthenaPawn(PC->MyFortPawn) && PC->MyFortPawn->Controller == PC)
        Pawn = PC->MyFortPawn;
    else if (IsLiveAthenaPawn(PC->Pawn))
        Pawn = PC->Pawn;
    else if (IsLiveAthenaPawn(PC->MyFortPawn))
        Pawn = PC->MyFortPawn;
    else
        return nullptr;

    PC->Pawn = Pawn;
    PC->MyFortPawn = Pawn;
    return Pawn;
}

void VersionFeatureAdapter::ForceLeaveAircraft(AFortPlayerControllerAthena* PC)
{
    auto ControllerClass = PlayerAIResolveCachedClass(PlayerAIControllerClassCache,
            "FortPlayerControllerAthena");

    if (!ControllerClass || !PlayerAIIsLiveSupportActor(PC) || !PC->IsA(ControllerClass))
        return;

    UFortControllerComponent_Aircraft* AircraftComponent = PlayerAITryGetAircraftComponent(PC);
    bool bWasAboard = IsInAircraft(PC);
    bool bMirrorAboard = false;

    if (AircraftComponent && PlayerAITryReadInAircraftBit(AircraftComponent, bMirrorAboard))
    {
        bWasAboard |= bMirrorAboard;
    }
    if (PlayerAITryReadInAircraftBit(PC, bMirrorAboard))
        bWasAboard |= bMirrorAboard;
    if (PC->PlayerState && PlayerAITryReadInAircraftBit(PC->PlayerState, bMirrorAboard))
        bWasAboard |= bMirrorAboard;

    auto Pawn = PlayerAIRefreshControllerPawn(PC);
    bool bCleared = PlayerAITryClearInAircraftBit(AircraftComponent);
    bCleared |= PlayerAITryClearInAircraftBit(PC);

    if (PC->PlayerState)
        bCleared |= PlayerAITryClearInAircraftBit(PC->PlayerState);

    const bool bExitLatchApplied = PlayerAITryApplyLegacyAircraftExitLatch(PC);

    PC->ForceNetUpdate();

    if (PC->PlayerState)
        PC->PlayerState->ForceNetUpdate();

    if (Pawn)
        Pawn->ForceNetUpdate();

    AIDebugLogger::Verbose("Transport",
        "force leave aircraft: was aboard: %d, pawn: %d, flag %s, legacy exit latch: %d, still aboard: %d",
        bWasAboard ? 1 : 0, Pawn ? 1 : 0, bCleared ? "cleared" : "not found",
        bExitLatchApplied ? 1 : 0, IsInAircraft(PC) ? 1 : 0);
}

static AFortPlayerControllerAthena* PlayerAIGetPawnController(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn || !PlayerAIIsLiveSupportActor(Pawn->Controller))
        return nullptr;

    auto ControllerClass = PlayerAIResolveCachedClass(PlayerAIControllerClassCache,
            "FortPlayerControllerAthena");

    return ControllerClass && Pawn->Controller->IsA(ControllerClass)
        ? (AFortPlayerControllerAthena*)Pawn->Controller : nullptr;
}

static bool PlayerAIHasObservedSkydiveState(AFortPlayerPawnAthena* Pawn, bool bWasInAircraft)
{
    if (!Pawn)
        return false;

    if ((Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) || (Pawn->HasbIsSkydivingFromBus() &&
         Pawn->bIsSkydivingFromBus))
    {
        return true;
    }

    auto PC = PlayerAIGetPawnController(Pawn);

    if (!PC)
        return false;

    const bool bNowInAircraft = VersionFeatureAdapter::IsInAircraft(PC);

    if (bWasInAircraft && !bNowInAircraft)
        return true;

    bool bGrounded = false;
    return !bNowInAircraft && VersionFeatureAdapter::TryIsPawnGrounded(Pawn, bGrounded) &&
        !bGrounded;
}

bool VersionFeatureAdapter::TryBeginSkydiving(AFortPlayerPawnAthena* Pawn)
{
    auto PawnClass = PlayerAIResolveCachedClass(PlayerAIPawnClassCache, "FortPlayerPawnAthena");

    if (!PawnClass || !PlayerAIIsLiveSupportActor(Pawn) || !Pawn->IsA(PawnClass))
        return false;

    auto Fn = Pawn->GetFunction("BeginSkydiving");

    if (!PlayerAIIsLiveSupportObject(Fn))
    {
        AIDebugLogger::MissingFeature("BeginSkydivingForPlayerAI",
            "no BeginSkydiving on this version - PlayerAI lands by direct placement");
        return false;
    }

    const auto Params = Fn->GetParamsNamed();
    const size_t BufferSize = Params.Size > 0 ? (size_t)Params.Size : 1;

    if (BufferSize > 0x1000)
    {
        AIDebugLogger::MissingFeature("BeginSkydivingForPlayerAI",
            "BeginSkydiving parameter layout exceeded the guarded buffer");
        return false;
    }

    int BoolOffset = -1;
    int FallbackBoolOffset = -1;

    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "bFromAircraft")
        {
            BoolOffset = (int)Param.Offset;
            break;
        }

        if ((Param.PropertyFlags & 0x400) == 0 && Param.ElementSize == 1 &&
            FallbackBoolOffset == -1)
        {
            FallbackBoolOffset = (int)Param.Offset;
        }
    }

    if (BoolOffset == -1)
        BoolOffset = FallbackBoolOffset;

    if (BoolOffset >= (int)BufferSize)
    {
        AIDebugLogger::MissingFeature("BeginSkydivingForPlayerAI",
            "bFromAircraft could not be resolved inside the guarded buffer");
        return false;
    }

    std::vector<uint8_t> Buffer(BufferSize, 0);

    if (BoolOffset >= 0)
        Buffer[(size_t)BoolOffset] = 1; // bFromAircraft = true

    auto PC = PlayerAIGetPawnController(Pawn);
    const bool bWasInAircraft = PC && IsInAircraft(PC);

    if (!PlayerAIGuardedProcessEvent(Pawn, Fn, Buffer.data()))
        return false;

    return PlayerAIHasObservedSkydiveState(Pawn, bWasInAircraft);
}

static bool PlayerAITryGroundTrace(UWorld* World, AFortPlayerPawnAthena* IgnorePawn,
    const FVector& Near, FVector& OutGround)
{
    GGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        OutGround = UFortKismetLibrary::FindGroundLocationAt(World, IgnorePawn, FVector(Near),
            10000.f, -10000.f, FName());
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GGuardedNativeCallDepth--;
    return bOk;
}

static bool bGroundTraceDisabled = false;
static int GroundTraceCalls = 0;
static int GroundTraceHits = 0;
static uint32_t PlayerAILandingProbeCursor = 0;

static bool PlayerAIUpdateGroundTraceReliability()
{
    if (bGroundTraceDisabled)
        return false;

    if (GroundTraceCalls >= 20 && (int64_t)GroundTraceHits * 10 <(int64_t)GroundTraceCalls * 3)
    {
        bGroundTraceDisabled = true;
        AIDebugLogger::MissingFeature("GroundTraceForPlayerAI",
            "ground trace rarely finds terrain on this version - PlayerAI runs trace-free");
        return false;
    }

    return true;
}

bool VersionFeatureAdapter::IsGroundTraceReliable()
{
    return PlayerAIUpdateGroundTraceReliability();
}

FVector VersionFeatureAdapter::FindGroundLocation(const FVector& Near, bool& bOutFound,
    AFortPlayerPawnAthena* IgnorePawn)
{
    bOutFound = false;

    if (!PlayerAIUpdateGroundTraceReliability())
        return Near;

    if (PlayerAIGroundTraceBudgetRemaining <= 0)
        return Near;

    auto World = UWorld::GetWorld();

    if (!World)
        return Near;

    PlayerAIGroundTraceBudgetRemaining--;

    FVector Ground{};

    if (!PlayerAITryGroundTrace(World, IgnorePawn, Near, Ground))
    {
        bGroundTraceDisabled = true;
        AIDebugLogger::MissingFeature("GroundTraceForPlayerAI",
            "native ground trace faulted and was disabled - PlayerAI runs trace-free");
        return Near;
    }

    GroundTraceCalls++;
    const bool bFoundGround = Ground.X != 0.f || Ground.Y != 0.f || Ground.Z != 0.f;

    if (bFoundGround)
        GroundTraceHits++;

    if (!PlayerAIUpdateGroundTraceReliability() || !bFoundGround)
        return Near;

    bOutFound = true;
    return Ground;
}

bool VersionFeatureAdapter::TryResolveGroundedLandingSpot(const FVector& Desired,
    AFortPlayerPawnAthena* IgnorePawn, FVector& OutSpot)
{
    OutSpot = FVector{};

    if (!std::isfinite((double)Desired.X) || !std::isfinite((double)Desired.Y) ||
        !std::isfinite((double)Desired.Z))
    {
        return false;
    }

    bool bFound = false;
    auto Ground = FindGroundLocation(Desired, bFound, IgnorePawn);

    if (bFound)
    {
        OutSpot = Ground;
        return true;
    }

    constexpr float Radii[] =
    {
        500.f, 1000.f, 2000.f,
    };
    constexpr float Directions[][2] =
    {
        { 1.f, 0.f },
        { 0.70710678f, 0.70710678f },
        { 0.f, 1.f },
        { -0.70710678f, 0.70710678f },
        { -1.f, 0.f },
        { -0.70710678f, -0.70710678f },
        { 0.f, -1.f },
        { 0.70710678f, -0.70710678f },
    };

    if (!PlayerAIUpdateGroundTraceReliability() || PlayerAIGroundTraceBudgetRemaining <= 0)
    {
        return false;
    }

    constexpr uint32_t RingCount = (uint32_t)(sizeof(Radii) / sizeof(Radii[0]));
    constexpr uint32_t DirectionCount = (uint32_t)(sizeof(Directions) / sizeof(Directions[0]));
    constexpr uint32_t ProbeCount = RingCount * DirectionCount;

    const uint32_t ProbeIndex = PlayerAILandingProbeCursor++ % ProbeCount;
    const uint32_t Ring = ProbeIndex / DirectionCount;
    const uint32_t Step = ProbeIndex % DirectionCount;
    const uint32_t Direction = (Step + Ring * 3) % DirectionCount;
    FVector Probe = Desired;
    Probe.X += Directions[Direction][0] * Radii[Ring];
    Probe.Y += Directions[Direction][1] * Radii[Ring];

    Ground = FindGroundLocation(Probe, bFound, IgnorePawn);

    if (bFound)
    {
        OutSpot = Ground;
        return true;
    }

    return false;
}

static bool PlayerAITryReadWalkingMovementMode(UCharacterMovementComponent* Movement,
    bool& OutGrounded)
{
    bool bRead = false;
    GGuardedNativeCallDepth++;

    __try
    {
        const uint32 Offset = Movement->GetOffset("MovementMode");

        if (Offset != (uint32)-1 && Offset < 0x10000 && SDK::MemReadable(
                (const uint8_t*)Movement + Offset, 1))
        {
            const uint8_t Mode = GetFromOffset<uint8_t>(Movement, Offset);

            if (Mode <= 7)
            {
                OutGrounded = Mode == 1 || Mode == 2;
                bRead = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bRead = false;
    }

    GGuardedNativeCallDepth--;
    return bRead;
}

bool VersionFeatureAdapter::TryIsPawnGrounded(AFortPlayerPawnAthena* Pawn, bool& OutGrounded)
{
    OutGrounded = false;
    auto PawnClass = PlayerAIResolveCachedClass(PlayerAIPawnClassCache, "FortPlayerPawnAthena");

    if (!PawnClass || !PlayerAIIsLiveSupportActor(Pawn) || !Pawn->IsA(PawnClass) ||
        !Pawn->HasCharacterMovement())
    {
        return false;
    }

    auto Movement = Pawn->CharacterMovement;
    auto MovementClass = PlayerAIResolveCachedClass(PlayerAIMovementComponentClassCache,
            "CharacterMovementComponent");

    if (!MovementClass || !PlayerAIIsLiveSupportObject(Movement) || !Movement->IsA(MovementClass))
    {
        return false;
    }

    auto Function = Movement->GetFunction("IsMovingOnGround");

    if (!PlayerAIIsLiveSupportObject(Function))
        return PlayerAITryReadWalkingMovementMode(Movement, OutGrounded);

    const auto Params = Function->GetParamsNamed();
    const size_t BufferSize = Params.Size > 0 ? (size_t)Params.Size : 1;

    if (BufferSize > 0x1000)
        return PlayerAITryReadWalkingMovementMode(Movement, OutGrounded);

    int ReturnOffset = -1;

    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "ReturnValue")
        {
            if (Param.ElementSize != 1 || (Param.PropertyFlags & 0x400) == 0)
            {
                return PlayerAITryReadWalkingMovementMode(Movement, OutGrounded);
            }

            ReturnOffset = (int)Param.Offset;
            continue;
        }

        if ((Param.PropertyFlags & 0x80) != 0)
        {
            return PlayerAITryReadWalkingMovementMode(Movement, OutGrounded);
        }
    }

    if (ReturnOffset < 0 || ReturnOffset >= (int)BufferSize)
    {
        return PlayerAITryReadWalkingMovementMode(Movement, OutGrounded);
    }

    std::vector<uint8_t> Buffer(BufferSize, 0);

    if (!PlayerAIGuardedProcessEvent(Movement, Function, Buffer.data()))
    {
        return PlayerAITryReadWalkingMovementMode(Movement, OutGrounded);
    }

    OutGrounded = Buffer[(size_t)ReturnOffset] != 0;
    return true;
}

bool VersionFeatureAdapter::SupportsCrouch(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn)
        return false;

    static int Supported = -1;

    if (Supported == -1)
        Supported = Pawn->GetFunction("Crouch") != nullptr ? 1 : 0;

    return Supported == 1;
}

bool VersionFeatureAdapter::SupportsGliding()
{
    return GetAircraft() != nullptr;
}

static bool PlayerAIIsLiveSafeZoneIndicator(AFortSafeZoneIndicator* Indicator)
{
    if (!PlayerAIIsLiveSupportActor(Indicator))
        return false;

    auto IndicatorClass = PlayerAIResolveCachedClass(PlayerAISafeZoneIndicatorClassCache,
            "FortSafeZoneIndicator");

    return IndicatorClass && Indicator->IsA(IndicatorClass);
}

static AFortSafeZoneIndicator* PlayerAIResolveSafeZoneIndicator()
{
    if (auto PhaseLogic = PlayerAIResolvePhaseLogic())
    {
        if (PhaseLogic->HasSafeZoneIndicator())
        {
            auto Indicator = PhaseLogic->SafeZoneIndicator;

            if (PlayerAIIsLiveSafeZoneIndicator(Indicator))
            {
                return Indicator;
            }
        }
    }

    auto GameMode = VersionFeatureAdapter::GetGameMode();

    if (!PlayerAIIsLiveSupportActor(GameMode) || !GameMode->HasSafeZoneIndicator())
    {
        return nullptr;
    }

    auto Indicator = GameMode->SafeZoneIndicator;
    return PlayerAIIsLiveSafeZoneIndicator(Indicator) ? Indicator : nullptr;
}

bool VersionFeatureAdapter::TryGetSafeZone(FVector& OutCenter, float& OutRadius)
{
    OutCenter = FVector{};
    OutRadius = 0.f;
    auto Indicator = PlayerAIResolveSafeZoneIndicator();

    if (!Indicator)
        return false;

    if (Indicator->HasNextCenter() && Indicator->HasNextRadius())
    {
        OutCenter = Indicator->NextCenter;
        OutRadius = Indicator->NextRadius;
        return std::isfinite((double)OutCenter.X) && std::isfinite((double)OutCenter.Y) &&
            std::isfinite((double)OutCenter.Z) && std::isfinite((double)OutRadius) &&
            OutRadius > 0.f;
    }

    // Old builds.
    if (Indicator->HasLastCenter() && Indicator->HasRadius())
    {
        OutCenter = Indicator->LastCenter;
        OutRadius = Indicator->Radius;
        return std::isfinite((double)OutCenter.X) && std::isfinite((double)OutCenter.Y) &&
            std::isfinite((double)OutCenter.Z) && std::isfinite((double)OutRadius) &&
            OutRadius > 0.f;
    }

    return false;
}

static bool PlayerAITryIsInCurrentSafeZone(const UObject* Owner, const FVector& Location,
    bool& OutInside)
{
    if (!PlayerAIIsLiveSupportObject(Owner))
        return false;

    auto Function = Owner->GetFunction("IsInCurrentSafeZone");

    if (!PlayerAIIsLiveSupportObject(Function))
        return false;

    bool bCalled = false;
    GGuardedNativeCallDepth++;

    __try
    {
        OutInside = Owner->Call<bool>(Function, Location, false);
        bCalled = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    GGuardedNativeCallDepth--;
    return bCalled;
}

bool VersionFeatureAdapter::IsInsideSafeZone(const FVector& Location)
{
    auto GameMode = GetGameMode();
    auto PhaseLogic = PlayerAIResolvePhaseLogic();
    auto Indicator = PlayerAIResolveSafeZoneIndicator();

    const bool bHasComponentIndicator = PhaseLogic && PhaseLogic->HasSafeZoneIndicator() &&
        PhaseLogic->SafeZoneIndicator == Indicator;
    const bool bHasGameModeIndicator = PlayerAIIsLiveSupportActor(GameMode) &&
        GameMode->HasSafeZoneIndicator() && GameMode->SafeZoneIndicator == Indicator;

    if (!Indicator)
        return true; // no storm yet - everywhere is safe

    bool bInside = true;

    if (bHasComponentIndicator && PlayerAITryIsInCurrentSafeZone(PhaseLogic, Location, bInside))
    {
        return bInside;
    }

    if (bHasGameModeIndicator && PlayerAITryIsInCurrentSafeZone(GameMode, Location, bInside))
    {
        return bInside;
    }

    FVector Center{};
    float Radius = 0.f;

    if (!TryGetSafeZone(Center, Radius))
        return true;

    FVector Flat = Location;
    Flat.Z = Center.Z;
    return Flat.GetDistanceTo(Center) <= Radius;
}

bool VersionFeatureAdapter::IsStormClosed(float TimeSeconds)
{
    auto Indicator = PlayerAIResolveSafeZoneIndicator();

    if (!Indicator || !Indicator->HasSafeZoneFinishShrinkTime())
    {
        return false;
    }

    const float Finish = Indicator->SafeZoneFinishShrinkTime;
    return std::isfinite((double)Finish) && Finish > 1.f && TimeSeconds >= Finish;
}

float VersionFeatureAdapter::GetStormDamagePerSecond()
{
    auto GameMode = GetGameMode();
    auto Indicator = PlayerAIResolveSafeZoneIndicator();

    if (!PlayerAIIsLiveSupportActor(GameMode))
        GameMode = nullptr;

    int Phase = 1;

    if (GameMode)
    {
        if (GameMode->HasSafeZonePhase() && GameMode->SafeZonePhase > 0)
            Phase = GameMode->SafeZonePhase;
        else if (Indicator && Indicator->HasCurrentPhase())
            Phase = Indicator->CurrentPhase;
    }
    else if (Indicator && Indicator->HasCurrentPhase())
        Phase = Indicator->CurrentPhase;

    if (Indicator && Indicator->HasCurrentDamageInfo() && FFortSafeZoneDamageInfo::HasDamage())
    {
        float Damage = Indicator->CurrentDamageInfo.Damage;

        if (std::isfinite((double)Damage) && Damage > 0.f)
        {
            if (FFortSafeZoneDamageInfo::HasbPercentageBasedDamage() &&
                Indicator->CurrentDamageInfo.bPercentageBasedDamage)
                Damage *= 100.f;

            if (Damage < 1.f)
                Damage = 1.f;
            if (Damage > 12.f)
                Damage = 12.f;

            return Damage;
        }
    }

    // Generic per-phase defaults.
    static const float PhaseDamage[] = { 1.f, 1.f, 1.f, 2.f, 5.f, 7.f, 8.f, 10.f, 10.f, 10.f };
    const int Index = Phase < 0 ? 0 : (Phase > 9 ? 9 : Phase);
    return PhaseDamage[Index];
}

bool VersionFeatureAdapter::SupportsDBNO()
{
    auto GameState = GetGameState();

    if (!GameState)
        return false;

    if (GameState->HasbDBNOEnabledForGameMode())
        return GameState->bDBNOEnabledForGameMode;

    return false;
}

bool VersionFeatureAdapter::KillPawn(AFortPlayerPawnAthena* Pawn,
    AFortPlayerControllerAthena* KillerPC, AActor* DamageCauser)
{
    if (!Pawn)
        return false;

    // TODO: route this through the Magnesium damage system if a GE-based kill is preferred on newer versions.
    auto ForceKillFn = Pawn->GetFunction("ForceKill");

    if (ForceKillFn)
    {
        FGameplayTag DeathTag{};
        Pawn->Call<void>(ForceKillFn, DeathTag, KillerPC, DamageCauser);
        return true;
    }

    AIDebugLogger::MissingFeature("ForceKill", "fatal PlayerAI damage was skipped safely");
    AIDebugLogger::Error("Damage",
        "fatal damage skipped: ForceKill unavailable pawn=%p killer=%p causer=%p",
        (void*)Pawn, (void*)KillerPC, (void*)DamageCauser);
    return false;
}

// Chapter 1 replicated six cosmetic slots; modern builds add Gameplay and ExtraPart.
static constexpr int32 PlayerAILegacyCharacterPartSlotCount = 6;
static constexpr int32 PlayerAICharacterPartSlotCount = 8;

static int SkinCacheAttempts = 0;
static std::vector<UAthenaCharacterItemDefinition*> CachedSkins;
static std::unordered_set<UAthenaCharacterItemDefinition*> CachedSkinObjects;
static std::unordered_map<std::string, UAthenaCharacterItemDefinition*> CachedSkinsByLowerName;
struct FPlayerAIResolvedSkinParts
{
    int32 CharacterObjectIndex = -1;
    bool bAllAuthoredPartsResolved = false;
    const UObject* Parts[PlayerAICharacterPartSlotCount]{};
    int32 PartObjectIndices[PlayerAICharacterPartSlotCount]
    {
        -1, -1, -1, -1, -1, -1, -1, -1,
    };
};
static std::unordered_map<UAthenaCharacterItemDefinition*, FPlayerAIResolvedSkinParts>
    CachedResolvedSkinParts;
static std::vector<UAthenaCharacterItemDefinition*> CachedResolvedSkinPool;
struct FPlayerAISkinCatalogEntry
{
    uint8 RawPrimaryAssetId[0x10]{};
    int32 PrimaryAssetIdSize = 0;
    std::string Name;
};
static std::vector<FPlayerAISkinCatalogEntry> CachedSkinCatalog;
static std::unordered_map<std::string, size_t> CachedSkinCatalogByLowerName;
enum class EPlayerAIPendingRandomSkinPhase : uint8
{
    ResolveCharacter, BaseParts, Specializations, SpecializationParts, Ready,
};
struct FPlayerAIPendingRandomSkin
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
    TWeakObjectPtr<AFortPlayerStateAthena> PlayerState;
    TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
    AFortPlayerPawnAthena* PawnIdentity = nullptr;
    FPlayerAISkinCatalogEntry CatalogEntry;
    TWeakObjectPtr<UObject> Character;
    TWeakObjectPtr<UObject> Specialization;
    TWeakObjectPtr<UObject> ResolvedParts[PlayerAICharacterPartSlotCount];
    ULONGLONG QueuedAt = 0;
    ULONGLONG WorkStartedAt = 0;
    ULONGLONG CandidateStartedAt = 0;
    ULONGLONG RetryAt = 0;
    int CandidateAttempts = 0;
    int BasePartCursor = 0;
    int SpecializationCursor = 0;
    int SpecializationPartCursor = 0;
    int PartReferencesVisited = 0;
    size_t ResidentSelectionStart = 0;
    size_t ResidentSelectionCursor = 0;
    bool bHasBaseParts = false;
    uint8 ResolvedPartMask = 0;
    bool bHasHead = false;
    bool bHasBody = false;
    bool bUseResidentFallback = false;
    bool bResidentSelectionInitialized = false;
    EPlayerAIPendingRandomSkinPhase Phase = EPlayerAIPendingRandomSkinPhase::ResolveCharacter;
};
static std::vector<FPlayerAIPendingRandomSkin> PendingRandomBotSkins;
struct FPlayerAIPendingRequestedSkinCommit
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
    TWeakObjectPtr<AFortPlayerStateAthena> PlayerState;
    TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
    AFortPlayerPawnAthena* PawnIdentity = nullptr;
    TWeakObjectPtr<UObject> Character;
    ULONGLONG QueuedAt = 0;
    ULONGLONG WorkStartedAt = 0;
    ULONGLONG RetryAt = 0;
    bool bUseDefault = false;
};
static std::vector<FPlayerAIPendingRequestedSkinCommit> PendingRequestedBotSkinCommits;
static std::unordered_map<AFortPlayerPawnAthena*, size_t> PendingBotSkinCommitPawnCounts;
static std::unordered_set<AFortPlayerPawnAthena*> PendingBotSkinActivePawns;
static uint64 PlayerAIBotSkinProgressGeneration = 1;
static void PlayerAINoteBotSkinProgress()
{
    PlayerAIBotSkinProgressGeneration++;
    if (!PlayerAIBotSkinProgressGeneration)
        PlayerAIBotSkinProgressGeneration = 1;
}
struct FPlayerAIBotSkinSettle
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
    ULONGLONG Until = 0;
};
static std::unordered_map<AFortPlayerPawnAthena*, FPlayerAIBotSkinSettle> PendingBotSkinSettles;
static void PlayerAICancelPendingCheatBotVisualCommit(AFortPlayerPawnAthena* Pawn);
static void PlayerAITrackPendingSkinCommit(AFortPlayerPawnAthena* Pawn)
{
    if (Pawn)
    {
        PendingBotSkinSettles.erase(Pawn);
        PendingBotSkinCommitPawnCounts[Pawn]++;
    }
}

static void PlayerAIMarkPendingSkinCommitActive(AFortPlayerPawnAthena* Pawn)
{
    if (Pawn && PendingBotSkinActivePawns.insert(Pawn).second)
        PlayerAINoteBotSkinProgress();
}

static void PlayerAIReleasePendingSkinCommit(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn)
        return;

    auto Existing = PendingBotSkinCommitPawnCounts.find(Pawn);
    if (Existing == PendingBotSkinCommitPawnCounts.end())
        return;
    if (Existing->second > 1)
        Existing->second--;
    else
    {
        PendingBotSkinCommitPawnCounts.erase(Existing);
        PendingBotSkinActivePawns.erase(Pawn);
        PlayerAICancelPendingCheatBotVisualCommit(Pawn);
    }
    PlayerAINoteBotSkinProgress();
}

static size_t RandomSkinCatalogShuffleStart = 0;
static size_t RandomSkinCatalogShuffleCursor = 0;
static size_t RandomSkinCatalogShuffleSize = 0;
static std::string LastQueuedRandomSkinName;
static size_t PendingRandomBotSkinCursor = 0;
static constexpr size_t PlayerAIMaxPendingRandomBotSkins = 256;
static constexpr int PlayerAIRandomSkinRecordsPerTick = 2;
static constexpr int PlayerAIRandomSkinSoftResolvesPerRecordTick = 1;
static constexpr int PlayerAIRandomSkinMaxPartReferences = 256;
static constexpr int PlayerAIRandomSkinCandidatesPerBot = 8;
static constexpr ULONGLONG PlayerAIRandomSkinCandidateLifetimeMs = 15000ULL;
static constexpr ULONGLONG PlayerAIRandomSkinLifetimeMs = 30000ULL;
static constexpr ULONGLONG PlayerAIRandomSkinCatalogWaitMs = 5000ULL;
static uint8* PendingSkinCatalogData = nullptr;
static int32 PendingSkinCatalogCount = 0;
static int32 PendingSkinCatalogIdSize = 0;
static int32 PendingSkinCatalogNameSize = 0;
static int32 PendingSkinCatalogCursor = 0;
static std::vector<FPlayerAISkinCatalogEntry> PendingSkinCatalogEntries;
static std::unordered_set<std::string> PendingSkinCatalogSeenNames;
static std::unordered_map<std::string, size_t> PendingSkinCatalogByLowerName;
static bool bCosmeticPathLoadDisabled = false;
static bool bCosmeticPackageLoadDisabled = false;
static bool bSoftCosmeticResolveDisabled = false;
static bool bSoftCosmeticLoadDisabled = false;
static bool bPrimaryAssetCosmeticFunctionsDisabled = false;
static bool bKnownSkinsLoaded = false;
static bool bLoadedSkinScanCompleted = false;
static bool bSkinCatalogReady = false;
static int SkinCatalogAttempts = 0;
static int SkinCatalogRetryTicks = 0;
static float LastSkinCatalogAttemptServerTime = -1.f;
static int KnownSkinScanCursor = 0;
static int LoadedSkinScanCursor = 0;
static int LoadedSkinScanLimit = 0;
static bool bDefaultCommandoResolved = false;
static const UObject* PlayerAIDefaultCommando = nullptr;
static bool bDefaultPartsResolved = false;
static const UObject* PlayerAIDefaultHead = nullptr;
static const UObject* PlayerAIDefaultBody = nullptr;
static const UObject* PlayerAIDefaultBackpack = nullptr;
static const UObject* PlayerAIResidentPlaceholderParts[PlayerAICharacterPartSlotCount]{};
static int32 PlayerAIResidentPlaceholderPartIndices[PlayerAICharacterPartSlotCount]
{
    -1, -1, -1, -1, -1, -1, -1, -1,
};
static const UObject* PlayerAIResidentPlaceholderHero = nullptr;
static int32 PlayerAIResidentPlaceholderHeroIndex = -1;
static size_t PlayerAIResidentPlaceholderDonorCursor = 0;
static ULONGLONG PlayerAIResidentPlaceholderRetryAt = 0;
static bool bPlayerAIServerChoosePartDisabled = false;
static bool bPlayerAIServerSetCosmeticLoadoutDisabled = false;
static bool bPlayerAIApplyCharacterCosmeticsDisabled = false;
static bool bPlayerAIRequestedDefaultSynchronousAttempted = false;

static void PlayerAICacheResolvedSkin(UAthenaCharacterItemDefinition* Character);
static UAthenaCharacterItemDefinition* PlayerAIValidateCharacterDefinition(const UObject* Object);
static void PlayerAITickPendingRandomBotSkins();
static bool PlayerAITickPendingRequestedBotSkinCommits();

static const UObject* PlayerAITryFindLoadedCosmetic(const wchar_t* Path, const UClass* Class)
{
    if (bCosmeticPathLoadDisabled || !Path || !Class || !Offsets::StaticFindObject)
        return nullptr;

    GGuardedNativeCallDepth++;

    const UObject* Result = nullptr;
    bool bFaulted = false;

    __try
    {
        Result = SDK::StaticFindObject(Path, Class);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bFaulted = true;
    }

    GGuardedNativeCallDepth--;

    if (bFaulted)
    {
        bCosmeticPathLoadDisabled = true;
        AIDebugLogger::MissingFeature("CosmeticAssetLoading",
            "resident cosmetic lookup faulted and was disabled");
    }

    return Result;
}

static const UObject* PlayerAITryLoadCosmetic(const wchar_t* Path, const UClass* Class)
{
    if (bCosmeticPackageLoadDisabled || !Path || !Class || !Offsets::StaticLoadObject)
        return nullptr;

    GGuardedNativeCallDepth++;
    const UObject* Result = nullptr;
    bool bFaulted = false;

    __try
    {
        Result = SDK::StaticLoadObject(Path, Class);

        if (Result && (!PlayerAIIsLiveSupportObject(Result) || !Result->IsA(Class)))
        {
            Result = nullptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Result = nullptr;
        bFaulted = true;
    }

    GGuardedNativeCallDepth--;

    if (bFaulted)
    {
        bCosmeticPackageLoadDisabled = true;
        AIDebugLogger::MissingFeature("SynchronousCosmeticLoading",
            "the requested cheat-bot skin faulted and default cosmetics will be used");
    }

    return Result;
}

static const UObject* PlayerAIResolveDefaultCosmetic(const wchar_t* Path, const UClass* Class,
    bool bAllowSynchronousLoad)
{
    auto Result = PlayerAITryFindLoadedCosmetic(Path, Class);

    if (!Result && bAllowSynchronousLoad)
        Result = PlayerAITryLoadCosmetic(Path, Class);

    return Result;
}

static bool PlayerAITryWriteSoftObjectPath(const FSoftObjectPtr* SoftObject,
    wchar_t* Output, size_t OutputSize);

static const UObject* PlayerAITryResolveLoadedSoftObject(
    FSoftObjectPtr& SoftObject, const UClass* Class)
{
    if (bSoftCosmeticResolveDisabled || !Class)
        return nullptr;

    GGuardedNativeCallDepth++;
    const UObject* Result = nullptr;
    bool bFaulted = false;

    __try
    {
        Result = SoftObject.WeakPtr.Get();

        if (Result && (!PlayerAIIsLiveSupportObject(Result) || !Result->IsA(Class)))
        {
            Result = nullptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Result = nullptr;
        bFaulted = true;
    }

    GGuardedNativeCallDepth--;

    if (!Result && !bFaulted)
    {
        wchar_t Path[2048]{};
        if (PlayerAITryWriteSoftObjectPath(&SoftObject, Path, sizeof(Path) / sizeof(Path[0])))
        {
            Result = PlayerAITryFindLoadedCosmetic(Path, Class);
        }
    }

    if (bFaulted)
    {
        bSoftCosmeticResolveDisabled = true;
        AIDebugLogger::MissingFeature("SoftCosmeticResolution",
            "resident soft cosmetic lookup faulted and was disabled");
    }

    return Result;
}

static const UObject* PlayerAITryResolveSoftObjectForCommand(
    FSoftObjectPtr& SoftObject, const UClass* Class)
{
    if (bSoftCosmeticLoadDisabled || !Class)
        return nullptr;

    auto WeakObject = SoftObject.WeakPtr.Get();
    if (PlayerAIIsLiveSupportObject(WeakObject) && WeakObject->IsA(Class))
    {
        return WeakObject;
    }

    wchar_t Path[2048]{};
    if (!PlayerAITryWriteSoftObjectPath(&SoftObject, Path, sizeof(Path) / sizeof(Path[0])))
    {
        return nullptr;
    }

    auto Result = PlayerAITryFindLoadedCosmetic(Path, Class);
    return Result ? Result : PlayerAITryLoadCosmetic(Path, Class);
}

static bool PlayerAIIsSafeReturnedSoftPath(const FString& Path)
{
    if (Path.NumElements < 0 || Path.MaxElements < Path.NumElements || Path.MaxElements > 2048)
    {
        return false;
    }

    if (!Path.Data)
        return Path.NumElements == 0 && Path.MaxElements == 0;

    const size_t ReadSize = Path.NumElements > 0
        ? static_cast<size_t>(Path.NumElements) * sizeof(wchar_t) : sizeof(wchar_t);

    if (!SDK::MemReadable(Path.Data, ReadSize))
        return false;

    return Path.NumElements == 0 || Path.Data[Path.NumElements - 1] == L'\0';
}

static void PlayerAIWriteSoftObjectPathUnsafe(const FSoftObjectPtr* SoftObject,
    wchar_t* Output, size_t OutputSize)
{
    if (!SoftObject || !Output || OutputSize == 0)
        return;

    UEAllocatedWString Path;

    if (VersionInfo.EngineVersion <= 4.16)
    {
        const auto& LegacyPath = *reinterpret_cast<const FString*>(
                reinterpret_cast<const uint8*>(SoftObject) + offsetof(FSoftObjectPtr, ObjectID));
        if (!PlayerAIIsSafeReturnedSoftPath(LegacyPath) || LegacyPath.NumElements <= 1)
        {
            return;
        }

        Path.assign(LegacyPath.Data, LegacyPath.Data + LegacyPath.NumElements - 1);
    }
    else if (VersionInfo.FortniteVersion >= 23.00)
    {
        const uint8* Value = reinterpret_cast<const uint8*>(SoftObject);
        const uint32 PackageOffset = VersionInfo.EngineVersion < 5.3 ? 0x10 : 0x08;
        const uint32 AssetOffset = VersionInfo.EngineVersion < 5.3 ? 0x14 : 0x0C;
        const uint32 SubPathOffset = VersionInfo.EngineVersion < 5.3 ? 0x18 : 0x10;
        const auto& PackageName = *reinterpret_cast<const FName*>(Value + PackageOffset);
        const auto& AssetName = *reinterpret_cast<const FName*>(Value + AssetOffset);
        const auto& SubPath = *reinterpret_cast<const FString*>(Value + SubPathOffset);

        if (!PackageName.IsValid() || !PlayerAIIsSafeReturnedSoftPath(SubPath))
        {
            return;
        }

        Path = PackageName.ToWString();
        if (AssetName.IsValid())
        {
            Path += L".";
            Path += AssetName.ToWString();
        }
        if (SubPath.NumElements > 1)
        {
            Path += L":";
            Path.append(SubPath.Data, SubPath.Data + SubPath.NumElements - 1);
        }
    }
    else
    {
        const auto& ObjectPath = SoftObject->ObjectID;
        if (!ObjectPath.AssetPathName.IsValid() || !PlayerAIIsSafeReturnedSoftPath(
                ObjectPath.SubPathString))
        {
            return;
        }

        Path = ObjectPath.AssetPathName.ToWString();
        if (ObjectPath.SubPathString.NumElements > 1)
        {
            Path += L":";
            Path.append(ObjectPath.SubPathString.Data, ObjectPath.SubPathString.Data +
                    ObjectPath.SubPathString.NumElements - 1);
        }
    }

    if (Path.empty() || Path[0] != L'/' || Path.size() >= OutputSize)
    {
        return;
    }

    wcsncpy_s(Output, OutputSize, Path.c_str(), _TRUNCATE);
}

static bool PlayerAITryWriteSoftObjectPath(const FSoftObjectPtr* SoftObject,
    wchar_t* Output, size_t OutputSize)
{
    if (bSoftCosmeticLoadDisabled || !SoftObject || !Output || OutputSize == 0)
    {
        return false;
    }

    Output[0] = L'\0';
    bool bFaulted = false;
    GGuardedNativeCallDepth++;

    __try
    {
        PlayerAIWriteSoftObjectPathUnsafe(SoftObject, Output, OutputSize);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bFaulted = true;
        Output[0] = L'\0';
    }

    GGuardedNativeCallDepth--;

    if (bFaulted)
    {
        bSoftCosmeticLoadDisabled = true;
        AIDebugLogger::MissingFeature("SynchronousSoftCosmeticLoading",
            "the requested outfit's soft path could not be decoded safely");
    }

    return Output[0] != L'\0';
}

enum class EPlayerAISoftReferencePathState : uint8
{
    Empty, Authored, Unsafe,
};

static EPlayerAISoftReferencePathState
PlayerAIGetSoftReferencePathState(const FSoftObjectPtr& SoftObject)
{
    if (bSoftCosmeticLoadDisabled)
        return EPlayerAISoftReferencePathState::Unsafe;

    wchar_t Path[2048]{};
    if (PlayerAITryWriteSoftObjectPath(&SoftObject, Path, sizeof(Path) / sizeof(Path[0])))
    {
        return EPlayerAISoftReferencePathState::Authored;
    }

    return bSoftCosmeticLoadDisabled ? EPlayerAISoftReferencePathState::Unsafe
        : EPlayerAISoftReferencePathState::Empty;
}

static void PlayerAIFreeReturnedSoftReferencePath(FSoftObjectPtr* SoftObject)
{
    if (!SoftObject)
        return;

    FString* OwnedPath = nullptr;

    if (VersionInfo.EngineVersion <= 4.16)
    {
        OwnedPath = reinterpret_cast<FString*>(reinterpret_cast<uint8*>(SoftObject) +
            offsetof(FSoftObjectPtr, ObjectID));
    }
    else if (VersionInfo.FortniteVersion >= 23.00)
    {
        OwnedPath = reinterpret_cast<FString*>(reinterpret_cast<uint8*>(SoftObject) +
            (VersionInfo.EngineVersion < 5.3 ? 0x18 : 0x10));
    }
    else
    {
        OwnedPath = &SoftObject->ObjectID.SubPathString;
    }

    __try
    {
        if (OwnedPath && OwnedPath->Data && PlayerAIIsSafeReturnedSoftPath(*OwnedPath))
        {
            OwnedPath->Free();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static const UObject* PlayerAITryResolveReturnedSoftObjectForCommand(
    FSoftObjectPtr* SoftObject, const UClass* Class)
{
    if (!SoftObject)
        return nullptr;

    const UObject* Result = nullptr;

    __try
    {
        Result = PlayerAITryResolveSoftObjectForCommand(*SoftObject, Class);
    }
    __finally
    {
        PlayerAIFreeReturnedSoftReferencePath(SoftObject);
    }

    return Result;
}

static bool PlayerAITryNativeCustomization(uint64_t NativeFn, AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn)
{
    GGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        ((void (*)(AActor*, AFortPlayerPawnAthena*)) NativeFn)(PlayerState, Pawn);
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GGuardedNativeCallDepth--;
    return bOk;
}

static bool PlayerAITryUpdateCharacterPartsVisualization(AFortPlayerStateAthena* PlayerState)
{
    if (!PlayerState)
        return false;

    GGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization(PlayerState);
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GGuardedNativeCallDepth--;
    return bOk && UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization__Ptr != nullptr;
}

// CharacterData.Parts is the replicated source; LocalCharacterParts is only a transient mirror.
static bool PlayerAIResolveFixedCharacterPartArray(UObject* Owner, const char* PropertyName,
    UObject**& OutParts, int32* OutPartArrayDimension = nullptr)
{
    OutParts = nullptr;
    if (OutPartArrayDimension)
        *OutPartArrayDimension = 0;

    if (!Owner || !Owner->Class || !PropertyName || Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto Property = Owner->GetProperty(PropertyName);
    if (!Property)
        return false;

    const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(Offsets::Offset_Internal,
            Offsets::ElementSize)) + sizeof(uint32);
    if (!SDK::MemReadable(Property, RequiredMetadataBytes))
    {
        return false;
    }

    const int32 PropertyOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            Property, Offsets::Offset_Internal)));
    const uint32 ElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
    const int32 ArrayDimension = GetFromOffset<int32>(
        Property, Offsets::ElementSize - sizeof(int32));
    const int32 OwnerSize = Owner->Class->GetPropertiesSize();

    if (PropertyOffset < 0 || ElementSize != sizeof(UObject*) ||
        ArrayDimension < PlayerAILegacyCharacterPartSlotCount || ArrayDimension > 16 ||
        OwnerSize <= PropertyOffset || static_cast<size_t>(ElementSize) * ArrayDimension >
            static_cast<size_t>(OwnerSize - PropertyOffset))
    {
        return false;
    }

    auto Parts = reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(Owner) + PropertyOffset);
    if (!SDK::MemReadable(Parts, static_cast<size_t>(ElementSize) * ArrayDimension))
    {
        return false;
    }

    OutParts = Parts;
    if (OutPartArrayDimension)
        *OutPartArrayDimension = ArrayDimension;
    return true;
}

static bool PlayerAIResolveStructCharacterPartArray(UObject* Owner, const char* PropertyName,
    const char* StructName, const wchar_t* StructPath, UObject**& OutParts,
    uint8** OutStructData = nullptr, const UStruct** OutStruct = nullptr,
    uint32* OutStructStorageSize = nullptr, int32* OutPartArrayDimension = nullptr)
{
    OutParts = nullptr;
    if (OutStructData)
        *OutStructData = nullptr;
    if (OutStruct)
        *OutStruct = nullptr;
    if (OutStructStorageSize)
        *OutStructStorageSize = 0;
    if (OutPartArrayDimension)
        *OutPartArrayDimension = 0;

    if (!Owner || !Owner->Class || !PropertyName || !StructName || !StructPath ||
        Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto OuterProperty = Owner->GetProperty(PropertyName, 0x100000);
    const UStruct* PartsStruct = nullptr;

    if (Offsets::StaticFindObject)
    {
        PartsStruct = reinterpret_cast<const UStruct*>(SDK::StaticFindObject(
                StructPath, UObject::StaticClass()));
    }

    if (!PlayerAIIsLiveSupportObject(reinterpret_cast<const UObject*>(PartsStruct)))
    {
        PartsStruct = FindStruct(StructName);
    }

    auto PartsStructObject = reinterpret_cast<const UObject*>(PartsStruct);
    if (!PlayerAIIsLiveSupportObject(PartsStructObject) || !PartsStructObject->Class ||
        PartsStructObject->Class->Name.ToUtf8() != "ScriptStruct")
    {
        return false;
    }

    auto PartsProperty = PartsStruct ? PartsStruct->GetProperty("Parts") : nullptr;
    if (!OuterProperty || !PartsStruct || !PartsProperty)
        return false;

    const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(Offsets::Offset_Internal,
            Offsets::ElementSize)) + sizeof(uint32);
    if (!SDK::MemReadable(OuterProperty, RequiredMetadataBytes) || !SDK::MemReadable(
            PartsProperty, RequiredMetadataBytes))
    {
        return false;
    }

    const int32 OuterOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            OuterProperty, Offsets::Offset_Internal)));
    const uint32 OuterSize = GetFromOffset<uint32>(OuterProperty, Offsets::ElementSize);
    const int32 OuterArrayDimension = GetFromOffset<int32>(OuterProperty,
        Offsets::ElementSize - sizeof(int32));
    const int32 PartsOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            PartsProperty, Offsets::Offset_Internal)));
    const uint32 PartElementSize = GetFromOffset<uint32>(PartsProperty, Offsets::ElementSize);
    const int32 PartArrayDimension = GetFromOffset<int32>(PartsProperty,
        Offsets::ElementSize - sizeof(int32));
    const int32 PartsStructSize = PartsStruct->GetPropertiesSize();
    const int32 OwnerSize = Owner->Class->GetPropertiesSize();
    const size_t RequiredPartsBytes = static_cast<size_t>(PartElementSize) * PartArrayDimension;

    if (OuterOffset < 0 || PartsOffset < 0 || OuterArrayDimension != 1 ||
        PartElementSize != sizeof(UObject*) ||
        PartArrayDimension < PlayerAILegacyCharacterPartSlotCount || PartArrayDimension > 16 ||
        PartsStructSize <= PartsOffset || RequiredPartsBytes > static_cast<size_t>(
            PartsStructSize - PartsOffset) || OuterSize < static_cast<uint32>(PartsOffset) ||
        RequiredPartsBytes > static_cast<size_t>(OuterSize - PartsOffset) ||
        OwnerSize <= OuterOffset || OuterSize > static_cast<uint32>(OwnerSize - OuterOffset))
    {
        return false;
    }

    auto Parts = reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(Owner) +
        OuterOffset + PartsOffset);
    if (!SDK::MemReadable(Parts, RequiredPartsBytes))
        return false;

    OutParts = Parts;
    if (OutStructData)
    {
        *OutStructData = reinterpret_cast<uint8*>(Owner) + OuterOffset;
    }
    if (OutStruct)
        *OutStruct = PartsStruct;
    if (OutStructStorageSize)
        *OutStructStorageSize = OuterSize;
    if (OutPartArrayDimension)
        *OutPartArrayDimension = PartArrayDimension;
    return true;
}

static bool PlayerAIResolveStructField(uint8* StructData, const UStruct* Struct,
    uint32 StructStorageSize, const char* FieldName, uint64 CastFlags, uint32 ExpectedElementSize,
    uint8*& OutAddress, const UField** OutProperty = nullptr)
{
    OutAddress = nullptr;
    if (OutProperty)
        *OutProperty = nullptr;

    if (!StructData || !Struct || !FieldName || ExpectedElementSize == 0 ||
        Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto Property = Struct->GetProperty(FieldName, CastFlags);
    if (!Property)
        return false;

    const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(Offsets::Offset_Internal,
            Offsets::ElementSize)) + sizeof(uint32);
    if (!SDK::MemReadable(Property, RequiredMetadataBytes))
    {
        return false;
    }

    const int32 FieldOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            Property, Offsets::Offset_Internal)));
    const uint32 FieldElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
    const int32 FieldArrayDimension = GetFromOffset<int32>(Property,
            Offsets::ElementSize - sizeof(int32));
    const int32 StructSize = Struct->GetPropertiesSize();

    if (FieldOffset < 0 || FieldElementSize != ExpectedElementSize || FieldArrayDimension != 1 ||
        StructSize <= FieldOffset || ExpectedElementSize > static_cast<uint32>(
            StructSize - FieldOffset) || StructStorageSize <= static_cast<uint32>(FieldOffset) ||
        ExpectedElementSize > StructStorageSize - FieldOffset)
    {
        return false;
    }

    auto Address = StructData + FieldOffset;
    if (!SDK::MemReadable(Address, ExpectedElementSize))
    {
        return false;
    }

    OutAddress = Address;
    if (OutProperty)
        *OutProperty = Property;
    return true;
}

static bool PlayerAIWriteStructBool(uint8* StructData, const UStruct* Struct,
    uint32 StructStorageSize, const char* FieldName, bool Value)
{
    uint8* Address = nullptr;
    const UField* Property = nullptr;
    if (!PlayerAIResolveStructField(StructData, Struct, StructStorageSize,
            FieldName, 0x20000, sizeof(uint8), Address, &Property))
    {
        return false;
    }

    uint8 FieldMask = 0;
    if (Offsets::FieldMask > 0 && SDK::MemReadable(Property,
            static_cast<size_t>(Offsets::FieldMask) + sizeof(uint8)))
    {
        FieldMask = Property->GetFieldMask();
    }

    if (!FieldMask)
        return false;

    Value ? *Address |= FieldMask : *Address &= ~FieldMask;

    return true;
}

static bool PlayerAIReadStructBool(uint8* StructData, const UStruct* Struct,
    uint32 StructStorageSize, const char* FieldName, bool& OutValue)
{
    OutValue = false;
    uint8* Address = nullptr;
    const UField* Property = nullptr;
    if (!PlayerAIResolveStructField(StructData, Struct, StructStorageSize,
            FieldName, 0x20000, sizeof(uint8), Address, &Property))
    {
        return false;
    }

    uint8 FieldMask = 0;
    if (Offsets::FieldMask > 0 && SDK::MemReadable(Property,
            static_cast<size_t>(Offsets::FieldMask) + sizeof(uint8)))
    {
        FieldMask = Property->GetFieldMask();
    }
    if (!FieldMask)
        return false;

    OutValue = (*Address & FieldMask) != 0;
    return true;
}

static void PlayerAIInitializeCharacterPartReplicationState(uint8* StructData,
    const UStruct* Struct, uint32 StructStorageSize, int32 PartArrayDimension,
    const UObject* Parts[PlayerAICharacterPartSlotCount])
{
    // 7.40 names the valid-part mask CustomCharacterParts and 10.40+ CustomCharacterData. A bit set for an empty slot makes clients reject the whole outfit.
    const char* FlagNames[] =
    {
        "WasReplicatedFlags", "WasPartReplicatedFlags",
    };

    const int32 ReplicatedSlotCount = (std::min)((std::max)(PartArrayDimension, 0),
        PlayerAICharacterPartSlotCount);
    uint8 ReplicatedPartMask = 0;
    if (Parts)
    {
        for (int32 Index = 0;
             Index < ReplicatedSlotCount; Index++)
        {
            if (Parts[Index])
            {
                ReplicatedPartMask |= static_cast<uint8>(1u << Index);
            }
        }
    }

    for (auto FlagName : FlagNames)
    {
        uint8* Flags = nullptr;
        if (PlayerAIResolveStructField(StructData, Struct, StructStorageSize,
                FlagName, 0, sizeof(uint8), Flags))
        {
            *Flags = ReplicatedPartMask;
        }
    }

    PlayerAIWriteStructBool(StructData, Struct, StructStorageSize, "bReplicationFailed", false);
}

static void PlayerAIWriteCharacterPartArray(UObject** Target, int32 TargetSlotCount,
    const UObject* Parts[PlayerAICharacterPartSlotCount])
{
    if (!Target || TargetSlotCount <= 0)
        return;

    const int32 SlotCount = (std::min)(TargetSlotCount, PlayerAICharacterPartSlotCount);
    for (int32 Index = 0;
         Index < SlotCount; Index++)
    {
        Target[Index] = const_cast<UObject*>(Parts[Index]);
    }
}

static bool PlayerAIWritePawnCharacterParts(AFortPlayerPawnAthena* Pawn,
    const UObject* Parts[PlayerAICharacterPartSlotCount])
{
    UObject** Target = nullptr;
    int32 TargetSlotCount = 0;

    if (!Pawn || !PlayerAIResolveFixedCharacterPartArray(Pawn, "CharacterParts", Target,
            &TargetSlotCount))
    {
        return false;
    }

    PlayerAIWriteCharacterPartArray(Target, TargetSlotCount, Parts);
    return true;
}

static bool PlayerAIWriteLocalCharacterParts(AFortPlayerStateAthena* PlayerState,
    const UObject* Parts[PlayerAICharacterPartSlotCount])
{
    UObject** Target = nullptr;
    int32 TargetSlotCount = 0;

    if (!PlayerState || !PlayerAIResolveFixedCharacterPartArray(
            PlayerState, "LocalCharacterParts", Target, &TargetSlotCount))
    {
        return false;
    }

    PlayerAIWriteCharacterPartArray(Target, TargetSlotCount, Parts);
    return true;
}

enum class EPlayerAICharacterPartMirrorState : uint8
{
    Unavailable, Matched, Mismatched,
};

static EPlayerAICharacterPartMirrorState
PlayerAIGetCharacterPartMirrorState(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, const UObject* Parts[PlayerAICharacterPartSlotCount])
{
    if (!Parts || !Parts[0] || !Parts[1])
        return EPlayerAICharacterPartMirrorState::Unavailable;

    bool bFoundMirror = false;
    auto MatchesRequestedParts = [&](UObject** Candidate, int32 CandidateSlotCount) -> bool
        {
            if (!Candidate || CandidateSlotCount <= 0)
                return false;

            bFoundMirror = true;
            for (int32 Index = 0;
                 Index < PlayerAICharacterPartSlotCount; Index++)
            {
                if (Index >= CandidateSlotCount)
                {
                    if (Parts[Index])
                        return false;
                    continue;
                }
                if (Candidate[Index] != Parts[Index])
                    return false;
            }
            return true;
        };

    UObject** Candidate = nullptr;
    int32 CandidateSlotCount = 0;
    if (Pawn && PlayerAIResolveFixedCharacterPartArray(Pawn, "CharacterParts", Candidate,
            &CandidateSlotCount) && MatchesRequestedParts(Candidate, CandidateSlotCount))
    {
        return EPlayerAICharacterPartMirrorState::Matched;
    }

    Candidate = nullptr;
    CandidateSlotCount = 0;
    if (PlayerState && PlayerAIResolveFixedCharacterPartArray(
            PlayerState, "LocalCharacterParts", Candidate, &CandidateSlotCount) &&
        MatchesRequestedParts(Candidate, CandidateSlotCount))
    {
        return EPlayerAICharacterPartMirrorState::Matched;
    }

    return bFoundMirror ? EPlayerAICharacterPartMirrorState::Mismatched
        : EPlayerAICharacterPartMirrorState::Unavailable;
}

// Pre-filling the pawn's own CharacterParts makes several Chapter 1 paths skip rebuilding the mesh.
static bool PlayerAIWriteReplicatedCharacterParts(AFortPlayerStateAthena* PlayerState,
    const UObject* Parts[PlayerAICharacterPartSlotCount])
{
    if (!PlayerState)
        return false;

    bool bWroteAuthoritativeState = false;
    UObject** Target = nullptr;
    uint8* StructData = nullptr;
    const UStruct* CharacterPartsStruct = nullptr;
    uint32 StructStorageSize = 0;
    int32 TargetSlotCount = 0;

    if (PlayerAIResolveStructCharacterPartArray(PlayerState, "CharacterData", "CustomCharacterData",
            L"/Script/FortniteGame.CustomCharacterData", Target, &StructData, &CharacterPartsStruct,
            &StructStorageSize, &TargetSlotCount))
    {
        PlayerAIWriteCharacterPartArray(Target, TargetSlotCount, Parts);
        PlayerAIInitializeCharacterPartReplicationState(StructData, CharacterPartsStruct,
            StructStorageSize, TargetSlotCount, Parts);
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, L"CharacterData");
        bWroteAuthoritativeState = true;
    }

    TargetSlotCount = 0;
    if (PlayerAIResolveFixedCharacterPartArray(PlayerState, "CharacterParts", Target,
            &TargetSlotCount))
    {
        PlayerAIWriteCharacterPartArray(Target, TargetSlotCount, Parts);
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, L"CharacterParts");
        bWroteAuthoritativeState = true;
    }
    else if (PlayerAIResolveStructCharacterPartArray(PlayerState, "CharacterParts",
                 "CustomCharacterParts", L"/Script/FortniteGame.CustomCharacterParts",
                 Target, &StructData, &CharacterPartsStruct, &StructStorageSize, &TargetSlotCount))
    {
        PlayerAIWriteCharacterPartArray(Target, TargetSlotCount, Parts);
        PlayerAIInitializeCharacterPartReplicationState(StructData, CharacterPartsStruct,
            StructStorageSize, TargetSlotCount, Parts);
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, L"CharacterParts");
        bWroteAuthoritativeState = true;
    }

    if (!bWroteAuthoritativeState)
    {
        AIDebugLogger::MissingFeature("ReplicatedCharacterParts",
            "PlayerAI keeps its engine default appearance");
    }

    return bWroteAuthoritativeState;
}

static void PlayerAIWriteHeroType(AFortPlayerStateAthena* PlayerState, const UObject* HeroType,
    bool bMarkDirty = true)
{
    if (!PlayerState || !PlayerState->HasHeroType())
        return;

    PlayerState->HeroType = HeroType;
    if (bMarkDirty)
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, L"HeroType");
    }
}

static bool PlayerAITryResolveEnumByteOffset(const UObject* Object, const char* PropertyName,
    uint32& OutOffset)
{
    OutOffset = (uint32)-1;
    if (!PlayerAIIsLiveSupportObject(Object) || !PropertyName || !Object->Class ||
        Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto Property = const_cast<UObject*>(Object)->GetProperty(PropertyName);
    if (!Property)
        return false;

    const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(Offsets::Offset_Internal,
            Offsets::ElementSize)) + sizeof(uint32);
    if (!SDK::MemReadable(Property, RequiredMetadataBytes))
        return false;

    const uint32 Offset = static_cast<uint32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            Property, Offsets::Offset_Internal)));
    const uint32 ElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
    const int32 ArrayDimension = GetFromOffset<int32>(
        Property, Offsets::ElementSize - sizeof(int32));
    const int32 ObjectSize = Object->Class->GetPropertiesSize();
    if (Offset == (uint32)-1 || Offset >= 0x10000 ||
        ElementSize != sizeof(uint8) || ArrayDimension != 1 ||
        ObjectSize <= 0 || Offset >= static_cast<uint32>(ObjectSize))
    {
        return false;
    }

    auto Address = reinterpret_cast<const uint8*>(Object) + Offset;
    if (!SDK::MemReadable(Address, sizeof(uint8)))
        return false;

    OutOffset = Offset;
    return true;
}

static bool PlayerAITryReadEnumByte(const UObject* Object, const char* PropertyName,
    uint8& OutValue, uint32* OutOffset = nullptr)
{
    uint32 Offset = (uint32)-1;
    if (!PlayerAITryResolveEnumByteOffset(Object, PropertyName, Offset))
    {
        return false;
    }

    OutValue = GetFromOffset<uint8>(Object, Offset);
    if (OutOffset)
        *OutOffset = Offset;
    return true;
}

static void PlayerAIWriteGender(AFortPlayerStateAthena* PlayerState, EFortCustomGender Gender,
    bool bMarkDirty = true)
{
    const uint8 RawGender = static_cast<uint8>(Gender);
    if (!PlayerState || (RawGender != 1 && RawGender != 2))
        return;

    auto GenderOffset = PlayerState->GetOffset("Gender");
    const wchar_t* DirtyProperty = L"Gender";

    if (GenderOffset == (uint32)-1)
    {
        GenderOffset = PlayerState->GetOffset("CharacterGender");
        DirtyProperty = L"CharacterGender";
    }

    if (GenderOffset == (uint32)-1 || GenderOffset >= 0x10000)
    {
        return;
    }

    GetFromOffset<EFortCustomGender>(PlayerState, GenderOffset) = Gender;
    if (bMarkDirty)
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, DirtyProperty);
    }

    uint32 LocalGenderOffset = (uint32)-1;
    if (PlayerAITryResolveEnumByteOffset(PlayerState, "LocalCharacterGender", LocalGenderOffset))
    {
        GetFromOffset<EFortCustomGender>(PlayerState, LocalGenderOffset) = Gender;
    }
}

static bool PlayerAIResolveConcreteGender(AFortPlayerStateAthena* PlayerState,
    UAthenaCharacterItemDefinition* Character, const UObject* Parts[PlayerAICharacterPartSlotCount],
    EFortCustomGender& OutGender)
{
    if (Character && Character->HasGender())
    {
        const uint8 RawGender = static_cast<uint8>(Character->Gender);
        if (RawGender == 1 || RawGender == 2)
        {
            OutGender = Character->Gender;
            return true;
        }
    }

    const int CorePartOrder[] = { 1, 0 };
    for (int PartIndex : CorePartOrder)
    {
        uint8 PermittedGender = 0;
        if (Parts && Parts[PartIndex] && PlayerAITryReadEnumByte(
                Parts[PartIndex], "GenderPermitted", PermittedGender) &&
            (PermittedGender == 1 || PermittedGender == 2))
        {
            OutGender = static_cast<EFortCustomGender>(PermittedGender);
            return true;
        }
    }

    uint8 ExistingGender = 0;
    if (PlayerState && ((PlayerAITryReadEnumByte(PlayerState, "CharacterGender", ExistingGender) ||
          PlayerAITryReadEnumByte(PlayerState, "Gender", ExistingGender))) &&
        (ExistingGender == 1 || ExistingGender == 2))
    {
        OutGender = static_cast<EFortCustomGender>(ExistingGender);
        return true;
    }

    return false;
}

static bool PlayerAIResolveConcreteBodyType(AFortPlayerStateAthena* PlayerState,
    const UObject* Parts[PlayerAICharacterPartSlotCount], uint8& OutBodyType)
{
    uint8 AllowedMask = 0x7;
    bool bFoundConstraint = false;
    const int CorePartOrder[] = { 1, 0 };
    for (int PartIndex : CorePartOrder)
    {
        uint8 PermittedBodyTypes = 0;
        if (!Parts || !Parts[PartIndex] || !PlayerAITryReadEnumByte(
                Parts[PartIndex], "BodyTypesPermitted", PermittedBodyTypes) ||
            PermittedBodyTypes > 6)
        {
            continue;
        }

        AllowedMask &= static_cast<uint8>(PermittedBodyTypes + 1);
        bFoundConstraint = true;
    }

    if (!bFoundConstraint || AllowedMask == 0)
        return false;

    if (AllowedMask == 0x1)
        OutBodyType = 0; // Small
    else if (AllowedMask == 0x2)
        OutBodyType = 1; // Medium
    else if (AllowedMask == 0x4)
        OutBodyType = 3; // Large
    else
    {
        uint8 ExistingBodyType = 0xff;
        if (!PlayerAITryReadEnumByte(PlayerState, "CharacterBodyType", ExistingBodyType))
        {
            PlayerAITryReadEnumByte(PlayerState, "LocalCharacterBodyType", ExistingBodyType);
        }

        uint8 ExistingMask = 0;
        if (ExistingBodyType == 0)
            ExistingMask = 0x1;
        else if (ExistingBodyType == 1)
            ExistingMask = 0x2;
        else if (ExistingBodyType == 3)
            ExistingMask = 0x4;
        if (!ExistingMask || !(AllowedMask & ExistingMask))
            return false;

        OutBodyType = ExistingBodyType;
    }

    return true;
}

static void PlayerAIWriteBodyType(AFortPlayerStateAthena* PlayerState, uint8 BodyType,
    bool bMarkDirty = true)
{
    if (!PlayerState || (BodyType != 0 && BodyType != 1 && BodyType != 3))
    {
        return;
    }

    uint32 BodyTypeOffset = (uint32)-1;
    const wchar_t* DirtyProperty = L"CharacterBodyType";
    if (!PlayerAITryResolveEnumByteOffset(PlayerState, "CharacterBodyType", BodyTypeOffset))
    {
        DirtyProperty = L"BodyType";
        if (!PlayerAITryResolveEnumByteOffset(PlayerState, "BodyType", BodyTypeOffset))
        {
            BodyTypeOffset = (uint32)-1;
        }
    }
    if (BodyTypeOffset != (uint32)-1)
    {
        GetFromOffset<uint8>(PlayerState, BodyTypeOffset) = BodyType;
        if (bMarkDirty)
        {
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, DirtyProperty);
        }
    }

    uint32 LocalBodyTypeOffset = (uint32)-1;
    if (PlayerAITryResolveEnumByteOffset(PlayerState, "LocalCharacterBodyType",
            LocalBodyTypeOffset))
    {
        GetFromOffset<uint8>(PlayerState, LocalBodyTypeOffset) = BodyType;
    }
}

static void PlayerAIChooseGenderOnPawn(AFortPlayerPawnAthena* Pawn, EFortCustomGender Gender)
{
    if (!Pawn)
        return;

    auto Function = Pawn->GetFunction("ServerChooseGender");
    if (!Function)
        return;

    uint8 Params = static_cast<uint8>(Gender);
    PlayerAIGuardedProcessEvent(Pawn, Function, &Params);
}

static bool PlayerAIChoosePartsOnPawn(AFortPlayerPawnAthena* Pawn,
    const UObject* Parts[PlayerAICharacterPartSlotCount], int* InOutPartCursor = nullptr,
    bool* OutAllPartsDispatched = nullptr)
{
    if (OutAllPartsDispatched)
        *OutAllPartsDispatched = false;
    if (!Pawn || !Parts || bPlayerAIServerChoosePartDisabled)
        return false;

    auto Function = Pawn->GetFunction("ServerChoosePart");
    if (!PlayerAIIsLiveSupportObject(Function))
        return false;

    const uint32 PartOffset = Function->GetOffset("Part");
    const uint32 ChosenPartOffset = Function->GetOffset("ChosenCharacterPart");
    if (PartOffset != 0 || ChosenPartOffset != 8)
    {
        bPlayerAIServerChoosePartDisabled = true;
        AIDebugLogger::MissingFeature("ServerChoosePartForPlayerAI",
            "the reflected parameter layout was unsupported; replicated character parts remain authoritative");
        return false;
    }

    const int32 ParamsSize = Function->GetPropertiesSize();
    if (ParamsSize < 0x10 || ParamsSize > 0x1000)
    {
        bPlayerAIServerChoosePartDisabled = true;
        AIDebugLogger::MissingFeature("ServerChoosePartForPlayerAI",
            "the parameter buffer size was unsupported; replicated character parts remain authoritative");
        return false;
    }

    alignas(16) uint8 Params[0x1000]{};
    bool bInvoked = false;
    const bool bCommitEmptyModernSlots = VersionInfo.FortniteVersion >= 19.0;
    const int SlotsToCommit = bCommitEmptyModernSlots
        ? (std::min)(PlayerAICharacterPartSlotCount, 6) : PlayerAICharacterPartSlotCount;
    int StartSlot = InOutPartCursor ? (std::max)(0, *InOutPartCursor) : 0;
    const int EndSlot = InOutPartCursor ? (std::min)(SlotsToCommit, StartSlot + 1) : SlotsToCommit;
    for (int i = StartSlot; i < EndSlot; i++)
    {
        auto Part = Parts[i];
        if (!Part && !bCommitEmptyModernSlots)
            continue;
        if (Part && !PlayerAIIsLiveSupportObject(Part))
            return false;

        memset(Params, 0, 0x10);
        Params[PartOffset] = static_cast<uint8>(i);
        auto MutablePart = const_cast<UObject*>(Part);
        memcpy(Params + ChosenPartOffset, &MutablePart, sizeof(MutablePart));

        if (!PlayerAIGuardedProcessEvent(Pawn, Function, Params))
        {
            bPlayerAIServerChoosePartDisabled = true;
            AIDebugLogger::MissingFeature("ServerChoosePartForPlayerAI",
                "the native part commit faulted and was disabled; reflected mirrors remain authoritative");
            return false;
        }
        bInvoked = true;
        if (InOutPartCursor)
            *InOutPartCursor = i + 1;
    }
    if (OutAllPartsDispatched)
    {
        *OutAllPartsDispatched = !InOutPartCursor || *InOutPartCursor >= SlotsToCommit;
    }
    return bInvoked;
}

// FortAthenaLoadout's Character sits at 0x40 in 10.40 and 0x48 in current layouts.
static bool PlayerAIWriteLoadoutCharacter(UObject* Owner, const char* PropertyName,
    const wchar_t* DirtyPropertyName, UAthenaCharacterItemDefinition* Character,
    uint8** OutLoadoutData = nullptr, uint32* OutLoadoutSize = nullptr,
    const UStruct** OutLoadoutStruct = nullptr, uint32* OutCharacterOffset = nullptr,
    bool bWriteCharacter = true, bool bMarkDirty = true)
{
    if (OutLoadoutData)
        *OutLoadoutData = nullptr;
    if (OutLoadoutSize)
        *OutLoadoutSize = 0;
    if (OutLoadoutStruct)
        *OutLoadoutStruct = nullptr;
    if (OutCharacterOffset)
        *OutCharacterOffset = 0;

    if (!Owner || !Owner->Class || !PropertyName || !DirtyPropertyName ||
        Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto OuterProperty = Owner->GetProperty(PropertyName, 0x100000);
    const UStruct* LoadoutStruct = PlayerAIIsLiveSupportObject(reinterpret_cast<const UObject*>(
                PlayerAIFortAthenaLoadoutStructCache)) ? PlayerAIFortAthenaLoadoutStructCache
        : nullptr;

    if (!LoadoutStruct && Offsets::StaticFindObject)
    {
        LoadoutStruct = reinterpret_cast<const UStruct*>(SDK::StaticFindObject(
                L"/Script/FortniteGame.FortAthenaLoadout", UObject::StaticClass()));
    }

    if (!PlayerAIIsLiveSupportObject(reinterpret_cast<const UObject*>(LoadoutStruct)))
    {
        LoadoutStruct = FindStruct("FortAthenaLoadout");
    }
    if (PlayerAIIsLiveSupportObject(reinterpret_cast<const UObject*>(LoadoutStruct)))
    {
        PlayerAIFortAthenaLoadoutStructCache = LoadoutStruct;
    }

    auto LoadoutStructObject = reinterpret_cast<const UObject*>(LoadoutStruct);
    if (!PlayerAIIsLiveSupportObject(LoadoutStructObject) || !LoadoutStructObject->Class ||
        LoadoutStructObject->Class->Name.ToUtf8() != "ScriptStruct")
    {
        return false;
    }

    auto CharacterProperty = LoadoutStruct ? LoadoutStruct->GetProperty("Character", 0x10000)
        : nullptr;
    if (!OuterProperty || !CharacterProperty)
        return false;

    const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(Offsets::Offset_Internal,
            Offsets::ElementSize)) + sizeof(uint32);
    if (!SDK::MemReadable(OuterProperty, RequiredMetadataBytes) || !SDK::MemReadable(
            CharacterProperty, RequiredMetadataBytes))
    {
        return false;
    }

    const int32 OuterOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            OuterProperty, Offsets::Offset_Internal)));
    const uint32 OuterSize = GetFromOffset<uint32>(OuterProperty, Offsets::ElementSize);
    const int32 OuterArrayDimension = GetFromOffset<int32>(OuterProperty,
        Offsets::ElementSize - sizeof(int32));
    const int32 CharacterOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            CharacterProperty, Offsets::Offset_Internal)));
    const uint32 CharacterSize = GetFromOffset<uint32>(CharacterProperty, Offsets::ElementSize);
    const int32 CharacterArrayDimension = GetFromOffset<int32>(CharacterProperty,
            Offsets::ElementSize - sizeof(int32));
    const int32 LoadoutStructSize = LoadoutStruct->GetPropertiesSize();
    const int32 OwnerSize = Owner->Class->GetPropertiesSize();

    if (OuterOffset < 0 || CharacterOffset < 0 || OuterArrayDimension != 1 ||
        CharacterArrayDimension != 1 || CharacterSize != sizeof(UObject*) ||
        LoadoutStructSize <= CharacterOffset || sizeof(UObject*) > static_cast<size_t>(
            LoadoutStructSize - CharacterOffset) ||
        OuterSize < static_cast<uint32>(CharacterOffset) || sizeof(UObject*) > static_cast<size_t>(
            OuterSize - CharacterOffset) || OwnerSize <= OuterOffset ||
        OuterSize > static_cast<uint32>(OwnerSize - OuterOffset))
    {
        return false;
    }

    auto LoadoutData = reinterpret_cast<uint8*>(Owner) + OuterOffset;
    auto Target = reinterpret_cast<UObject**>(LoadoutData + CharacterOffset);
    if (!SDK::MemReadable(Target, sizeof(UObject*)))
        return false;

    if (bWriteCharacter)
    {
        *Target = Character;
        PlayerAIWriteStructBool(LoadoutData, LoadoutStruct, OuterSize,
            "bIsDefaultCharacter", Character == nullptr);
        PlayerAIWriteStructBool(LoadoutData, LoadoutStruct, OuterSize,
            "bForceUpdateVariants", true);
        if (bMarkDirty)
        {
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(Owner, DirtyPropertyName);
        }
    }
    if (OutLoadoutData)
        *OutLoadoutData = LoadoutData;
    if (OutLoadoutSize)
        *OutLoadoutSize = OuterSize;
    if (OutLoadoutStruct)
        *OutLoadoutStruct = LoadoutStruct;
    if (OutCharacterOffset)
        *OutCharacterOffset = static_cast<uint32>(CharacterOffset);
    return true;
}

struct FPlayerAICharacterLoadoutSnapshot
{
    TWeakObjectPtr<UObject> Owner;
    const char* PropertyName = nullptr;
    const wchar_t* DirtyPropertyName = nullptr;
    TWeakObjectPtr<UObject> Character;
    bool bHadCharacter = false;
    bool bIsDefaultCharacter = false;
    bool bForceUpdateVariants = false;
    bool bHasIsDefaultCharacter = false;
    bool bHasForceUpdateVariants = false;
    bool bValid = false;
};

static FPlayerAICharacterLoadoutSnapshot
PlayerAICaptureCharacterLoadout(UObject* Owner, const char* PropertyName,
    const wchar_t* DirtyPropertyName)
{
    FPlayerAICharacterLoadoutSnapshot Snapshot;
    Snapshot.Owner = TWeakObjectPtr<UObject>(Owner);
    Snapshot.PropertyName = PropertyName;
    Snapshot.DirtyPropertyName = DirtyPropertyName;

    uint8* LoadoutData = nullptr;
    uint32 LoadoutSize = 0;
    const UStruct* LoadoutStruct = nullptr;
    uint32 CharacterOffset = 0;
    if (!PlayerAIWriteLoadoutCharacter(Owner, PropertyName, DirtyPropertyName, nullptr,
            &LoadoutData, &LoadoutSize, &LoadoutStruct, &CharacterOffset, false) ||
        !LoadoutData || CharacterOffset > LoadoutSize ||
        sizeof(UObject*) > LoadoutSize - CharacterOffset)
    {
        return Snapshot;
    }

    auto CharacterSlot = reinterpret_cast<UObject**>(LoadoutData + CharacterOffset);
    if (!SDK::MemReadable(CharacterSlot, sizeof(UObject*)))
        return Snapshot;

    auto Character = PlayerAIValidateCharacterDefinition(*CharacterSlot);
    if (*CharacterSlot && !Character)
        return Snapshot;

    Snapshot.Character = TWeakObjectPtr<UObject>(Character);
    Snapshot.bHadCharacter = Character != nullptr;
    Snapshot.bHasIsDefaultCharacter = PlayerAIReadStructBool(
            LoadoutData, LoadoutStruct, LoadoutSize, "bIsDefaultCharacter",
            Snapshot.bIsDefaultCharacter);
    Snapshot.bHasForceUpdateVariants = PlayerAIReadStructBool(
            LoadoutData, LoadoutStruct, LoadoutSize, "bForceUpdateVariants",
            Snapshot.bForceUpdateVariants);
    Snapshot.bValid = true;
    return Snapshot;
}

static void PlayerAIRestoreCharacterLoadout(const FPlayerAICharacterLoadoutSnapshot& Snapshot)
{
    auto Owner = Snapshot.Owner.Get();
    auto Character = PlayerAIValidateCharacterDefinition(Snapshot.Character.Get());
    if (!Snapshot.bValid || !PlayerAIIsLiveSupportObject(Owner) ||
        (Snapshot.bHadCharacter && !Character))
    {
        return;
    }

    uint8* LoadoutData = nullptr;
    uint32 LoadoutSize = 0;
    const UStruct* LoadoutStruct = nullptr;
    if (!PlayerAIWriteLoadoutCharacter(Owner, Snapshot.PropertyName, Snapshot.DirtyPropertyName,
        Character, &LoadoutData, &LoadoutSize, &LoadoutStruct, nullptr, true))
    {
        return;
    }

    if (Snapshot.bHasIsDefaultCharacter)
    {
        PlayerAIWriteStructBool(LoadoutData, LoadoutStruct, LoadoutSize, "bIsDefaultCharacter",
            Snapshot.bIsDefaultCharacter);
    }
    if (Snapshot.bHasForceUpdateVariants)
    {
        PlayerAIWriteStructBool(LoadoutData, LoadoutStruct, LoadoutSize, "bForceUpdateVariants",
            Snapshot.bForceUpdateVariants);
    }
    VersionFeatureAdapter::MarkReplicatedPropertyDirty(Owner, Snapshot.DirtyPropertyName);
}

struct FPlayerAIReflectedBoolSnapshot
{
    TWeakObjectPtr<UObject> Owner;
    const char* PropertyName = nullptr;
    const wchar_t* DirtyPropertyName = nullptr;
    bool Value = false;
    bool bValid = false;
};

static FPlayerAIReflectedBoolSnapshot
PlayerAICaptureReflectedBool(UObject* Owner, const char* PropertyName,
    const wchar_t* DirtyPropertyName)
{
    FPlayerAIReflectedBoolSnapshot Snapshot;
    Snapshot.Owner = TWeakObjectPtr<UObject>(Owner);
    Snapshot.PropertyName = PropertyName;
    Snapshot.DirtyPropertyName = DirtyPropertyName;
    Snapshot.bValid = PlayerAITryReadReflectedBool(Owner, PropertyName, Snapshot.Value);
    return Snapshot;
}

static void PlayerAIRestoreReflectedBool(const FPlayerAIReflectedBoolSnapshot& Snapshot)
{
    auto Owner = Snapshot.Owner.Get();
    if (!Snapshot.bValid || !PlayerAIIsLiveSupportObject(Owner) || !PlayerAISetReflectedReadyBool(
            Owner, Snapshot.PropertyName, Snapshot.Value))
    {
        return;
    }

    if (Snapshot.DirtyPropertyName)
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(Owner, Snapshot.DirtyPropertyName);
    }
}

struct FPlayerAIEnumByteSnapshot
{
    uint32 Offset = (uint32)-1;
    uint8 Value = 0;
    const wchar_t* DirtyProperty = nullptr;
    bool bValid = false;
};

static FPlayerAIEnumByteSnapshot PlayerAICaptureEnumByte(UObject* Owner, const char* PropertyName,
    const wchar_t* DirtyProperty)
{
    FPlayerAIEnumByteSnapshot Snapshot;
    Snapshot.DirtyProperty = DirtyProperty;
    Snapshot.bValid = PlayerAITryReadEnumByte(
        Owner, PropertyName, Snapshot.Value, &Snapshot.Offset);
    return Snapshot;
}

static void PlayerAIRestoreEnumByte(UObject* Owner, const FPlayerAIEnumByteSnapshot& Snapshot)
{
    if (!Snapshot.bValid || !PlayerAIIsLiveSupportObject(Owner) || !Owner->Class ||
        Snapshot.Offset >= static_cast<uint32>(Owner->Class->GetPropertiesSize()))
    {
        return;
    }

    GetFromOffset<uint8>(Owner, Snapshot.Offset) = Snapshot.Value;
    if (Snapshot.DirtyProperty)
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(Owner, Snapshot.DirtyProperty);
    }
}

struct FPlayerAICosmeticMetadataSnapshot
{
    TWeakObjectPtr<AFortPlayerStateAthena> PlayerState;
    TWeakObjectPtr<UObject> HeroType;
    bool bHadHeroTypeProperty = false;
    bool bHadHeroTypeValue = false;
    FPlayerAIEnumByteSnapshot ReplicatedGender;
    FPlayerAIEnumByteSnapshot LocalGender;
    FPlayerAIEnumByteSnapshot ReplicatedBodyType;
    FPlayerAIEnumByteSnapshot LocalBodyType;
};

static void PlayerAIRestoreCosmeticMetadata(const FPlayerAICosmeticMetadataSnapshot& Snapshot)
{
    auto PlayerState = Snapshot.PlayerState.Get();
    if (!PlayerAIIsLiveSupportObject(PlayerState))
        return;

    if (Snapshot.bHadHeroTypeProperty)
    {
        PlayerAIWriteHeroType(PlayerState, Snapshot.HeroType.Get());
    }
    PlayerAIRestoreEnumByte(PlayerState, Snapshot.ReplicatedGender);
    PlayerAIRestoreEnumByte(PlayerState, Snapshot.LocalGender);
    PlayerAIRestoreEnumByte(PlayerState, Snapshot.ReplicatedBodyType);
    PlayerAIRestoreEnumByte(PlayerState, Snapshot.LocalBodyType);
    PlayerState->ForceNetUpdate();
}

enum class EPlayerAICheatBotVisualCommitPhase : uint8
{
    AwaitNativeRefresh, DispatchExactParts, AwaitExactParts,
};

struct FPlayerAICharacterPartStoreSnapshot
{
    TWeakObjectPtr<UObject> Parts[PlayerAICharacterPartSlotCount];
    uint8 NonNullMask = 0;
    int32 SlotCount = 0;
    bool bPresent = false;
};

struct FPlayerAICharacterPartsSnapshot
{
    FPlayerAICharacterPartStoreSnapshot Pawn;
    FPlayerAICharacterPartStoreSnapshot Local;
    FPlayerAICharacterPartStoreSnapshot CharacterData;
    FPlayerAICharacterPartStoreSnapshot CharacterParts;
    bool bCharacterPartsIsStruct = false;
    bool bValid = false;
};

static bool PlayerAICaptureCharacterPartStore(UObject** Source, int32 SourceSlotCount,
    FPlayerAICharacterPartStoreSnapshot& Snapshot)
{
    if (!Source || SourceSlotCount < PlayerAILegacyCharacterPartSlotCount || SourceSlotCount > 16)
    {
        return false;
    }

    auto PartClass = UCustomCharacterPart::StaticClass();
    if (!PlayerAIIsLiveSupportObject(PartClass))
        return false;

    Snapshot.SlotCount = (std::min)(SourceSlotCount, PlayerAICharacterPartSlotCount);
    Snapshot.bPresent = true;
    for (int32 Index = 0; Index < Snapshot.SlotCount; Index++)
    {
        auto PartObject = Source[Index];
        if (!PartObject)
            continue;
        if (!PlayerAIIsLiveSupportObject(PartObject) || !PartObject->IsA(PartClass))
        {
            return false;
        }

        auto Part = reinterpret_cast<UCustomCharacterPart*>(PartObject);
        if (!Part->HasCharacterPartType() || Part->CharacterPartType != Index)
        {
            return false;
        }

        Snapshot.Parts[Index] = TWeakObjectPtr<UObject>(PartObject);
        Snapshot.NonNullMask |= static_cast<uint8>(1u << Index);
    }
    return true;
}

static FPlayerAICharacterPartsSnapshot
PlayerAICaptureCharacterParts(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    FPlayerAICharacterPartsSnapshot Snapshot;
    if (!PlayerAIIsLiveSupportObject(PlayerState) || !PlayerAIIsLiveSupportActor(Pawn))
    {
        return Snapshot;
    }

    UObject** Source = nullptr;
    int32 SourceSlotCount = 0;
    if (PlayerAIResolveFixedCharacterPartArray(Pawn, "CharacterParts", Source, &SourceSlotCount) &&
        !PlayerAICaptureCharacterPartStore(Source, SourceSlotCount, Snapshot.Pawn))
    {
        return FPlayerAICharacterPartsSnapshot{};
    }

    Source = nullptr;
    SourceSlotCount = 0;
    if (PlayerAIResolveFixedCharacterPartArray(PlayerState, "LocalCharacterParts", Source,
            &SourceSlotCount) && !PlayerAICaptureCharacterPartStore(
            Source, SourceSlotCount, Snapshot.Local))
    {
        return FPlayerAICharacterPartsSnapshot{};
    }

    uint8* StructData = nullptr;
    const UStruct* PartsStruct = nullptr;
    uint32 StructStorageSize = 0;
    Source = nullptr;
    SourceSlotCount = 0;
    if (PlayerAIResolveStructCharacterPartArray(PlayerState, "CharacterData", "CustomCharacterData",
            L"/Script/FortniteGame.CustomCharacterData", Source, &StructData, &PartsStruct,
            &StructStorageSize, &SourceSlotCount) && !PlayerAICaptureCharacterPartStore(
            Source, SourceSlotCount, Snapshot.CharacterData))
    {
        return FPlayerAICharacterPartsSnapshot{};
    }

    Source = nullptr;
    SourceSlotCount = 0;
    if (PlayerAIResolveFixedCharacterPartArray(PlayerState, "CharacterParts", Source,
            &SourceSlotCount))
    {
        if (!PlayerAICaptureCharacterPartStore(Source, SourceSlotCount, Snapshot.CharacterParts))
        {
            return FPlayerAICharacterPartsSnapshot{};
        }
    }
    else
    {
        StructData = nullptr;
        PartsStruct = nullptr;
        StructStorageSize = 0;
        if (PlayerAIResolveStructCharacterPartArray(PlayerState, "CharacterParts",
                "CustomCharacterParts", L"/Script/FortniteGame.CustomCharacterParts",
                Source, &StructData, &PartsStruct, &StructStorageSize, &SourceSlotCount))
        {
            if (!PlayerAICaptureCharacterPartStore(Source, SourceSlotCount,
                    Snapshot.CharacterParts))
            {
                return FPlayerAICharacterPartsSnapshot{};
            }
            Snapshot.bCharacterPartsIsStruct = true;
        }
    }

    Snapshot.bValid = Snapshot.CharacterData.bPresent || Snapshot.CharacterParts.bPresent;
    return Snapshot;
}

static bool PlayerAIResolveCharacterPartStoreSnapshot(
    const FPlayerAICharacterPartStoreSnapshot& Snapshot,
    const UObject* OutParts[PlayerAICharacterPartSlotCount])
{
    for (int Index = 0;
         Index < PlayerAICharacterPartSlotCount; Index++)
    {
        OutParts[Index] = nullptr;
        if (!(Snapshot.NonNullMask & static_cast<uint8>(1u << Index)))
        {
            continue;
        }

        OutParts[Index] = Snapshot.Parts[Index].Get();
        if (!PlayerAIIsLiveSupportObject(OutParts[Index]))
            return false;
    }
    return true;
}

static bool PlayerAIRestoreCharacterParts(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, const FPlayerAICharacterPartsSnapshot& Snapshot)
{
    if (!Snapshot.bValid || !PlayerAIIsLiveSupportObject(PlayerState) ||
        !PlayerAIIsLiveSupportActor(Pawn))
    {
        return false;
    }

    UObject** PawnTarget = nullptr;
    int32 PawnTargetCount = 0;
    UObject** LocalTarget = nullptr;
    int32 LocalTargetCount = 0;
    UObject** CharacterDataTarget = nullptr;
    int32 CharacterDataTargetCount = 0;
    uint8* CharacterData = nullptr;
    const UStruct* CharacterDataStruct = nullptr;
    uint32 CharacterDataSize = 0;
    UObject** CharacterPartsTarget = nullptr;
    int32 CharacterPartsTargetCount = 0;
    uint8* CharacterPartsData = nullptr;
    const UStruct* CharacterPartsStruct = nullptr;
    uint32 CharacterPartsDataSize = 0;

    if (Snapshot.Pawn.bPresent && !PlayerAIResolveFixedCharacterPartArray(
            Pawn, "CharacterParts", PawnTarget, &PawnTargetCount))
    {
        return false;
    }
    if (Snapshot.Local.bPresent && !PlayerAIResolveFixedCharacterPartArray(
            PlayerState, "LocalCharacterParts", LocalTarget, &LocalTargetCount))
    {
        return false;
    }
    if (Snapshot.CharacterData.bPresent && !PlayerAIResolveStructCharacterPartArray(
            PlayerState, "CharacterData", "CustomCharacterData",
            L"/Script/FortniteGame.CustomCharacterData", CharacterDataTarget, &CharacterData,
            &CharacterDataStruct, &CharacterDataSize, &CharacterDataTargetCount))
    {
        return false;
    }
    if (Snapshot.CharacterParts.bPresent)
    {
        const bool bResolvedCharacterParts = Snapshot.bCharacterPartsIsStruct
            ? PlayerAIResolveStructCharacterPartArray(PlayerState, "CharacterParts",
                "CustomCharacterParts", L"/Script/FortniteGame.CustomCharacterParts",
                CharacterPartsTarget, &CharacterPartsData, &CharacterPartsStruct,
                &CharacterPartsDataSize, &CharacterPartsTargetCount)
            : PlayerAIResolveFixedCharacterPartArray(PlayerState, "CharacterParts",
                CharacterPartsTarget, &CharacterPartsTargetCount);
        if (!bResolvedCharacterParts)
            return false;
    }

    const UObject* PawnParts[PlayerAICharacterPartSlotCount]{};
    const UObject* LocalParts[PlayerAICharacterPartSlotCount]{};
    const UObject* CharacterDataParts[PlayerAICharacterPartSlotCount]{};
    const UObject* CharacterParts[PlayerAICharacterPartSlotCount]{};
    if ((Snapshot.Pawn.bPresent && !PlayerAIResolveCharacterPartStoreSnapshot(
             Snapshot.Pawn, PawnParts)) || (Snapshot.Local.bPresent &&
         !PlayerAIResolveCharacterPartStoreSnapshot(Snapshot.Local, LocalParts)) ||
        (Snapshot.CharacterData.bPresent && !PlayerAIResolveCharacterPartStoreSnapshot(
             Snapshot.CharacterData, CharacterDataParts)) || (Snapshot.CharacterParts.bPresent &&
         !PlayerAIResolveCharacterPartStoreSnapshot(Snapshot.CharacterParts, CharacterParts)))
    {
        return false;
    }

    if (Snapshot.CharacterData.bPresent)
    {
        PlayerAIWriteCharacterPartArray(CharacterDataTarget, CharacterDataTargetCount,
            CharacterDataParts);
        PlayerAIInitializeCharacterPartReplicationState(CharacterData, CharacterDataStruct,
            CharacterDataSize, CharacterDataTargetCount, CharacterDataParts);
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, L"CharacterData");
    }
    if (Snapshot.CharacterParts.bPresent)
    {
        PlayerAIWriteCharacterPartArray(CharacterPartsTarget, CharacterPartsTargetCount,
            CharacterParts);
        if (Snapshot.bCharacterPartsIsStruct)
        {
            PlayerAIInitializeCharacterPartReplicationState(
                CharacterPartsData, CharacterPartsStruct, CharacterPartsDataSize,
                CharacterPartsTargetCount, CharacterParts);
        }
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, L"CharacterParts");
    }
    if (Snapshot.Local.bPresent)
    {
        PlayerAIWriteCharacterPartArray(LocalTarget, LocalTargetCount, LocalParts);
    }
    if (Snapshot.Pawn.bPresent)
    {
        PlayerAIWriteCharacterPartArray(PawnTarget, PawnTargetCount, PawnParts);
    }

    return true;
}

struct FPlayerAIPendingCheatBotVisualCommit
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
    TWeakObjectPtr<AFortPlayerStateAthena> PlayerState;
    TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
    TWeakObjectPtr<UObject> Character;
    TWeakObjectPtr<UObject> Parts[PlayerAICharacterPartSlotCount];
    uint8 NonNullPartMask = 0;
    FPlayerAICharacterLoadoutSnapshot PreviousLoadouts[6];
    FPlayerAIReflectedBoolSnapshot PreviousHeadAccessoryPreference;
    FPlayerAIReflectedBoolSnapshot PreviousBackpackPreference;
    FPlayerAICosmeticMetadataSnapshot PreviousMetadata;
    FPlayerAICharacterPartsSnapshot PreviousParts;
    ULONGLONG QueuedAt = 0;
    ULONGLONG NativeRefreshStartedAt = 0;
    ULONGLONG RetryAt = 0;
    ULONGLONG MirrorMatchedAt = 0;
    int ExactPartCursor = 0;
    bool bNativeLoadoutSetterAttempted = false;
    bool bApplyCosmeticsRejected = false;
    EPlayerAICheatBotVisualCommitPhase Phase =
        EPlayerAICheatBotVisualCommitPhase::AwaitNativeRefresh;
};

static std::unordered_map<AFortPlayerPawnAthena*, FPlayerAIPendingCheatBotVisualCommit>
    PendingCheatBotVisualCommits;
static bool bPlayerAILastCheatBotVisualCallPerformedNativeWork = false;

static bool PlayerAIRepublishPendingCheatBotVisualCommit(
    FPlayerAIPendingCheatBotVisualCommit& Pending);

static bool PlayerAIIsCheatBotVisualCommitPending(AFortPlayerPawnAthena* Pawn,
    UAthenaCharacterItemDefinition* Character)
{
    auto Existing = PendingCheatBotVisualCommits.find(Pawn);
    return Existing != PendingCheatBotVisualCommits.end() && Existing->second.Pawn.Get() == Pawn &&
        Existing->second.Character.Get() == Character;
}

static bool PlayerAICanRestorePendingCheatBotIdentity(
    const FPlayerAIPendingCheatBotVisualCommit& Pending)
{
    auto PlayerState = Pending.PlayerState.Get();
    auto Pawn = Pending.Pawn.Get();
    if (!PlayerAIIsLiveSupportObject(PlayerState) || !PlayerAIIsLiveSupportActor(Pawn))
    {
        return false;
    }

    for (const auto& Loadout : Pending.PreviousLoadouts)
    {
        if (!Loadout.bValid)
            continue;
        if (!PlayerAIIsLiveSupportObject(Loadout.Owner.Get()))
            return false;
        if (Loadout.bHadCharacter && !PlayerAIValidateCharacterDefinition(Loadout.Character.Get()))
        {
            return false;
        }
    }

    const auto& Metadata = Pending.PreviousMetadata;
    if (Metadata.PlayerState.Get() != PlayerState || (Metadata.bHadHeroTypeValue &&
         !PlayerAIIsLiveSupportObject(Metadata.HeroType.Get())))
    {
        return false;
    }
    if ((Pending.PreviousHeadAccessoryPreference.bValid && !PlayerAIIsLiveSupportObject(
             Pending.PreviousHeadAccessoryPreference.Owner.Get())) ||
        (Pending.PreviousBackpackPreference.bValid && !PlayerAIIsLiveSupportObject(
             Pending.PreviousBackpackPreference.Owner.Get())))
    {
        return false;
    }
    return true;
}

enum class EPlayerAICosmeticRecoveryResult : uint8
{
    PriorRestored, TargetRetained, Failed,
};

static EPlayerAICosmeticRecoveryResult
PlayerAIRestorePendingCheatBotVisualCommit(FPlayerAIPendingCheatBotVisualCommit& Pending)
{
    auto Pawn = Pending.Pawn.Get();
    EPlayerAICosmeticRecoveryResult Recovery = EPlayerAICosmeticRecoveryResult::Failed;
    const bool bRestoredPriorParts = PlayerAICanRestorePendingCheatBotIdentity(Pending) &&
        PlayerAIRestoreCharacterParts(Pending.PlayerState.Get(), Pending.Pawn.Get(),
            Pending.PreviousParts);
    if (bRestoredPriorParts)
    {
        Recovery = EPlayerAICosmeticRecoveryResult::PriorRestored;
        for (const auto& Loadout : Pending.PreviousLoadouts)
            PlayerAIRestoreCharacterLoadout(Loadout);
        PlayerAIRestoreCosmeticMetadata(Pending.PreviousMetadata);
        PlayerAIRestoreReflectedBool(Pending.PreviousHeadAccessoryPreference);
        PlayerAIRestoreReflectedBool(Pending.PreviousBackpackPreference);
        if (Pawn)
        {
            if (auto Reinitialized = Pawn->GetFunction("OnCharacterPartsReinitialized"))
            {
                VersionFeatureAdapter::SafeCallNoArgs(Pawn, Reinitialized);
            }
        }
    }
    else
    {
        const bool bRetainedSelectedState = PlayerAIRepublishPendingCheatBotVisualCommit(Pending);
        if (bRetainedSelectedState)
        {
            Recovery = EPlayerAICosmeticRecoveryResult::TargetRetained;
        }
        AIDebugLogger::MissingFeature("CheatBotCosmeticRollbackExpired", bRetainedSelectedState
                ? "a prior outfit object expired during the visual transaction; the selected CID was retained consistently"
                : "a prior outfit object expired and neither cosmetic state could be republished safely");
    }

    auto World = Pending.World.Get();
    if (Recovery != EPlayerAICosmeticRecoveryResult::Failed &&
        Pawn && World && World == UWorld::GetWorld() && PlayerAIIsLiveSupportActor(Pawn))
    {
        FPlayerAIBotSkinSettle Settle{};
        Settle.World = TWeakObjectPtr<UWorld>(World);
        Settle.Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
        Settle.Until = GetTickCount64() + 3000ULL;
        PendingBotSkinSettles[Pawn] = Settle;
    }

    if (auto PlayerState = Pending.PlayerState.Get())
        PlayerState->ForceNetUpdate();
    if (Pawn)
        Pawn->ForceNetUpdate();
    if (auto Controller = Pending.Controller.Get())
        Controller->ForceNetUpdate();
    return Recovery;
}

static void PlayerAICancelPendingCheatBotVisualCommit(AFortPlayerPawnAthena* Pawn)
{
    auto Existing = PendingCheatBotVisualCommits.find(Pawn);
    if (Existing == PendingCheatBotVisualCommits.end())
        return;

    PlayerAIRestorePendingCheatBotVisualCommit(Existing->second);
    PendingCheatBotVisualCommits.erase(Existing);
}

static void PlayerAIWriteCharacterLoadout(AFortPlayerPawnAthena* Pawn,
    UAthenaCharacterItemDefinition* Character, bool bWriteControllerLoadouts = true,
    bool bMarkDirty = true)
{
    if (!Pawn || !Character)
        return;

    bool bWrotePawnLoadout = PlayerAIWriteLoadoutCharacter(Pawn, "CosmeticLoadout",
            L"CosmeticLoadout", Character, nullptr, nullptr, nullptr, nullptr, true, bMarkDirty);
    // UE5 splits the default and currently-applied loadout into separate FortAthenaLoadout fields.
    bWrotePawnLoadout |= PlayerAIWriteLoadoutCharacter(Pawn, "BaseCosmeticLoadout",
        L"BaseCosmeticLoadout", Character, nullptr, nullptr, nullptr, nullptr, true, bMarkDirty);
    bWrotePawnLoadout |= PlayerAIWriteLoadoutCharacter(Pawn, "AppliedCosmeticLoadout",
        L"AppliedCosmeticLoadout", Character, nullptr, nullptr, nullptr, nullptr, true, bMarkDirty);

    if (!bWrotePawnLoadout)
    {
        AIDebugLogger::MissingFeature("PawnCosmeticLoadoutForPlayerAI",
            "CID still applies through replicated character parts");
    }

    if (!bWriteControllerLoadouts || !Pawn->HasController() || !Pawn->Controller)
        return;

    // Native Athena bot controllers inherit AAIController on 10.40/13.40 but still expose CosmeticLoadoutBC.
    auto Controller = Pawn->Controller;
    if (!PlayerAIIsLiveSupportObject(Controller))
        return;

    PlayerAIWriteLoadoutCharacter(Controller, "CosmeticLoadoutPC", L"CosmeticLoadoutPC", Character,
        nullptr, nullptr, nullptr, nullptr, true, bMarkDirty);
    PlayerAIWriteLoadoutCharacter(Controller, "CustomizationLoadout",
        L"CustomizationLoadout", Character, nullptr, nullptr, nullptr, nullptr, true, bMarkDirty);
    PlayerAIWriteLoadoutCharacter(Controller, "CosmeticLoadoutBC", L"CosmeticLoadoutBC", Character,
        nullptr, nullptr, nullptr, nullptr, true, bMarkDirty);
}

static bool PlayerAIRepublishPendingCheatBotVisualCommit(
    FPlayerAIPendingCheatBotVisualCommit& Pending)
{
    auto World = Pending.World.Get();
    auto PlayerState = Pending.PlayerState.Get();
    auto Pawn = Pending.Pawn.Get();
    auto Controller = Pending.Controller.Get();
    auto Character = PlayerAIValidateCharacterDefinition(Pending.Character.Get());
    auto PartClass = UCustomCharacterPart::StaticClass();
    if (!World || World != UWorld::GetWorld() || !PlayerAIIsLiveSupportObject(PlayerState) ||
        !PlayerAIIsLiveSupportActor(Pawn) || !PlayerAIIsLiveSupportObject(Controller) ||
        !Character || !PlayerAIIsLiveSupportObject(PartClass) ||
        Controller->PlayerState != PlayerState || Controller->Pawn != Pawn ||
        (Controller->HasMyFortPawn() && Controller->MyFortPawn != Pawn) ||
        Pawn->PlayerState != PlayerState || !Pawn->HasController() ||
        Pawn->Controller != Controller || (Pawn->HasbActorIsBeingDestroyed() &&
         Pawn->bActorIsBeingDestroyed) || (Pawn->HasbIsDying() && Pawn->bIsDying) ||
        (Pawn->HasbIsDBNO() && Pawn->bIsDBNO) || !AFortPlayerControllerAthena::
            IsCheatSpawnedBotController(Controller))
    {
        return false;
    }

    const uint8 RequiredMask = static_cast<uint8>((1u << 0) | (1u << 1));
    if ((Pending.NonNullPartMask & RequiredMask) != RequiredMask)
        return false;

    const UObject* Parts[PlayerAICharacterPartSlotCount]{};
    for (int Index = 0;
         Index < PlayerAICharacterPartSlotCount; Index++)
    {
        const bool bExpectedPart = (Pending.NonNullPartMask & static_cast<uint8>(1u << Index)) != 0;
        auto PartObject = Pending.Parts[Index].Get();
        if (!bExpectedPart)
        {
            if (PartObject)
                return false;
            continue;
        }
        if (!PlayerAIIsLiveSupportObject(PartObject) || !PartObject->IsA(PartClass))
        {
            return false;
        }
        auto Part = reinterpret_cast<UCustomCharacterPart*>(PartObject);
        if (!Part->HasCharacterPartType() || Part->CharacterPartType != Index)
        {
            return false;
        }
        Parts[Index] = PartObject;
    }

    PlayerAIWriteCharacterLoadout(Pawn, Character, true, true);
    if (!PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts))
    {
        return false;
    }
    PlayerAIWriteLocalCharacterParts(PlayerState, Parts);
    PlayerAIWritePawnCharacterParts(Pawn, Parts);

    if (Character->HasHeroDefinition() && PlayerAIIsLiveSupportObject(
            reinterpret_cast<const UObject*>(Character->HeroDefinition)))
    {
        PlayerAIWriteHeroType(PlayerState, reinterpret_cast<const UObject*>(
                Character->HeroDefinition));
    }
    EFortCustomGender Gender = EFortCustomGender::Invalid;
    if (PlayerAIResolveConcreteGender(PlayerState, Character, Parts, Gender))
    {
        PlayerAIWriteGender(PlayerState, Gender, true);
    }
    uint8 BodyType = 0;
    if (PlayerAIResolveConcreteBodyType(PlayerState, Parts, BodyType))
    {
        PlayerAIWriteBodyType(PlayerState, BodyType, true);
    }
    if (PlayerAISetReflectedReadyBool(PlayerState, "bShowHeroHeadAccessories", true))
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(
            PlayerState, L"bShowHeroHeadAccessories");
    }
    if (PlayerAISetReflectedReadyBool(PlayerState, "bShowHeroBackpack", true))
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, L"bShowHeroBackpack");
    }

    if (auto Reinitialized = Pawn->GetFunction("OnCharacterPartsReinitialized"))
    {
        VersionFeatureAdapter::SafeCallNoArgs(Pawn, Reinitialized);
    }
    PlayerState->FlushNetDormancy();
    PlayerState->ForceNetUpdate();
    Pawn->FlushNetDormancy();
    Pawn->ForceNetUpdate();
    Controller->FlushNetDormancy();
    Controller->ForceNetUpdate();
    return true;
}

static bool PlayerAITryReadObjectProperty(UObject* Owner, const char* PropertyName,
    UObject*& OutValue, UObject*** OutSlot = nullptr)
{
    OutValue = nullptr;
    if (OutSlot)
        *OutSlot = nullptr;
    if (!Owner || !Owner->Class || !PropertyName || Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto Property = Owner->GetProperty(PropertyName);
    if (!Property)
        return false;

    const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(Offsets::Offset_Internal,
            Offsets::ElementSize)) + sizeof(uint32);
    if (!SDK::MemReadable(Property, RequiredMetadataBytes))
        return false;

    const int32 PropertyOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
            Property, Offsets::Offset_Internal)));
    const uint32 ElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
    const int32 ArrayDimension = GetFromOffset<int32>(
        Property, Offsets::ElementSize - sizeof(int32));
    const int32 OwnerSize = Owner->Class->GetPropertiesSize();
    if (PropertyOffset < 0 || ElementSize != sizeof(UObject*) || ArrayDimension != 1 ||
        OwnerSize <= PropertyOffset || sizeof(UObject*) > static_cast<size_t>(
            OwnerSize - PropertyOffset))
    {
        return false;
    }

    auto Slot = reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(Owner) + PropertyOffset);
    if (!SDK::MemReadable(Slot, sizeof(UObject*)))
        return false;

    OutValue = *Slot;
    if (OutSlot)
        *OutSlot = Slot;
    return true;
}

static bool PlayerAITryCallNoArgBool(UObject* Object, const char* FunctionName, bool& OutValue)
{
    OutValue = false;
    if (!PlayerAIIsLiveSupportObject(Object) || !FunctionName)
        return false;

    auto Function = Object->GetFunction(FunctionName);
    if (!PlayerAIIsLiveSupportObject(Function))
        return false;

    const auto Params = Function->GetParamsNamed();
    if (Params.Size == 0 || Params.Size > 0x100)
        return false;

    constexpr uint64 CPF_Parm = 0x80;
    constexpr uint64 CPF_ReturnParm = 0x400;
    const UFunction::ParamNamed* ReturnParam = nullptr;
    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "ReturnValue")
        {
            if (Param.ElementSize != sizeof(bool) || !(Param.PropertyFlags & CPF_Parm) ||
                !(Param.PropertyFlags & CPF_ReturnParm) || Param.Offset >= Params.Size)
            {
                return false;
            }
            ReturnParam = &Param;
        }
        else if (Param.PropertyFlags & CPF_Parm)
        {
            return false;
        }
    }
    if (!ReturnParam)
        return false;

    alignas(16) uint8 Buffer[0x100]{};
    if (!PlayerAIGuardedProcessEvent(Object, Function, Buffer))
    {
        return false;
    }

    OutValue = Buffer[ReturnParam->Offset] != 0;
    return true;
}

static UObject* PlayerAITryCallNoArgObject(UObject* Object, const char* FunctionName,
    const UClass* ExpectedClass, bool& OutFunctionAvailable)
{
    OutFunctionAvailable = false;
    if (!PlayerAIIsLiveSupportObject(Object) || !FunctionName)
        return nullptr;

    auto Function = Object->GetFunction(FunctionName);
    if (!PlayerAIIsLiveSupportObject(Function))
        return nullptr;

    const auto Params = Function->GetParamsNamed();
    if (Params.Size == 0 || Params.Size > 0x100)
        return nullptr;

    constexpr uint64 CPF_Parm = 0x80;
    constexpr uint64 CPF_ReturnParm = 0x400;
    const UFunction::ParamNamed* ReturnParam = nullptr;
    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "ReturnValue")
        {
            if (Param.ElementSize != sizeof(UObject*) || !(Param.PropertyFlags & CPF_Parm) ||
                !(Param.PropertyFlags & CPF_ReturnParm) || Param.Offset > Params.Size ||
                sizeof(UObject*) > Params.Size - Param.Offset)
            {
                return nullptr;
            }
            ReturnParam = &Param;
        }
        else if (Param.PropertyFlags & CPF_Parm)
        {
            return nullptr;
        }
    }
    if (!ReturnParam)
        return nullptr;

    alignas(16) uint8 Buffer[0x100]{};
    if (!PlayerAIGuardedProcessEvent(Object, Function, Buffer))
    {
        return nullptr;
    }

    OutFunctionAvailable = true;
    UObject* Result = nullptr;
    memcpy(&Result, Buffer + ReturnParam->Offset, sizeof(Result));
    if (!PlayerAIIsLiveSupportObject(Result) || (ExpectedClass && !Result->IsA(ExpectedClass)))
    {
        return nullptr;
    }
    return Result;
}

static UObject* PlayerAITryGetCharacterPartMeshComponent(AFortPlayerPawnAthena* Pawn,
    uint8 PartType, bool& OutFunctionAvailable)
{
    OutFunctionAvailable = false;
    if (!PlayerAIIsLiveSupportActor(Pawn))
        return nullptr;

    auto Function = Pawn->GetFunction("GetSkeletalMeshForPartType");
    if (!PlayerAIIsLiveSupportObject(Function))
        return nullptr;

    const auto Params = Function->GetParamsNamed();
    if (Params.Size == 0 || Params.Size > 0x100)
        return nullptr;

    constexpr uint64 CPF_Parm = 0x80;
    constexpr uint64 CPF_ReturnParm = 0x400;
    const UFunction::ParamNamed* PartParam = nullptr;
    const UFunction::ParamNamed* ReturnParam = nullptr;
    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "ReturnValue")
            ReturnParam = &Param;
        else if (Param.Name == "PartType" || Param.Name == "CharacterPartType")
            PartParam = &Param;
        else if (Param.PropertyFlags & CPF_Parm)
            return nullptr;
    }

    if (!PartParam || !ReturnParam || PartParam->ElementSize != sizeof(uint8) ||
        ReturnParam->ElementSize != sizeof(UObject*) || !(PartParam->PropertyFlags & CPF_Parm) ||
        (PartParam->PropertyFlags & CPF_ReturnParm) || !(ReturnParam->PropertyFlags & CPF_Parm) ||
        !(ReturnParam->PropertyFlags & CPF_ReturnParm) || PartParam->Offset >= Params.Size ||
        ReturnParam->Offset > Params.Size || sizeof(UObject*) > Params.Size - ReturnParam->Offset)
    {
        return nullptr;
    }
    OutFunctionAvailable = true;

    alignas(16) uint8 Buffer[0x100]{};
    Buffer[PartParam->Offset] = PartType;
    if (!PlayerAIGuardedProcessEvent(Pawn, Function, Buffer))
    {
        return nullptr;
    }

    UObject* Component = nullptr;
    memcpy(&Component, Buffer + ReturnParam->Offset, sizeof(Component));
    return PlayerAIIsLiveSupportObject(Component) ? Component : nullptr;
}

enum class EPlayerAICharacterPartMeshMatch : uint8
{
    Unavailable, Empty, Matched, Mismatched,
};

static EPlayerAICharacterPartMeshMatch
PlayerAIMatchRenderedCharacterPartMesh(const UObject* Part, const UObject* RenderedMesh,
    const UClass* MeshClass)
{
    if (!PlayerAIIsLiveSupportObject(Part) || !Part->Class ||
        !PlayerAIIsLiveSupportObject(MeshClass))
    {
        return EPlayerAICharacterPartMeshMatch::Unavailable;
    }

    bool bHasAuthoredMeshIdentity = false;
    bool bInspectedMeshSource = false;
    bool bUnsafeMeshSource = false;
    auto TestCandidate = [&] (const UObject* Candidate) -> bool
        {
            if (!PlayerAIIsLiveSupportObject(Candidate) || !Candidate->IsA(MeshClass))
            {
                return false;
            }
            bHasAuthoredMeshIdentity = true;
            return Candidate == RenderedMesh;
        };

    bool bGetterAvailable = false;
    auto GetterMesh = PlayerAITryCallNoArgObject(const_cast<UObject*>(Part), "GetSkeletalMesh",
        MeshClass, bGetterAvailable);
    bInspectedMeshSource |= bGetterAvailable;
    if (TestCandidate(GetterMesh))
        return EPlayerAICharacterPartMeshMatch::Matched;

    const char* MeshProperties[] =
    {
        "SkeletalMesh", "SkeletalMeshOverride", "SkeletalMeshAsset", "Mesh",
    };
    for (const auto PropertyName : MeshProperties)
    {
        UObject* DirectMesh = nullptr;
        const bool bDirectPropertyAvailable = PlayerAITryReadObjectProperty(
                const_cast<UObject*>(Part), PropertyName, DirectMesh);
        bInspectedMeshSource |= bDirectPropertyAvailable;
        if (bDirectPropertyAvailable && DirectMesh && (!PlayerAIIsLiveSupportObject(DirectMesh) ||
             !DirectMesh->IsA(MeshClass)))
        {
            bUnsafeMeshSource = true;
        }
        if (bDirectPropertyAvailable && TestCandidate(DirectMesh))
        {
            return EPlayerAICharacterPartMeshMatch::Matched;
        }

        auto Property = Part->GetProperty(PropertyName, 0x20000000);
        if (!Property || Offsets::ElementSize < sizeof(int32))
        {
            continue;
        }

        const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(
                Offsets::Offset_Internal, Offsets::ElementSize)) + sizeof(uint32);
        if (!SDK::MemReadable(Property, RequiredMetadataBytes))
        {
            continue;
        }

        const int32 PropertyOffset = static_cast<int32>(
            SDK::ReadPropertyOffset(GetFromOffset<uint32>(Property, Offsets::Offset_Internal)));
        const uint32 PropertySize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
        const int32 ArrayDimension = GetFromOffset<int32>(Property,
            Offsets::ElementSize - sizeof(int32));
        const int32 OwnerSize = Part->Class->GetPropertiesSize();
        if (PropertyOffset < 0 || PropertySize != FSoftObjectPtr::Size() || ArrayDimension != 1 ||
            OwnerSize <= PropertyOffset || PropertySize > static_cast<uint32>(
                OwnerSize - PropertyOffset))
        {
            continue;
        }

        auto SoftMesh = reinterpret_cast<FSoftObjectPtr*>(reinterpret_cast<uint8*>(
                const_cast<UObject*>(Part)) + PropertyOffset);
        if (!SDK::MemReadable(SoftMesh, PropertySize))
            continue;

        bInspectedMeshSource = true;
        const auto PathState = PlayerAIGetSoftReferencePathState(*SoftMesh);
        if (PathState == EPlayerAISoftReferencePathState::Empty)
        {
            continue;
        }
        if (PathState == EPlayerAISoftReferencePathState::Unsafe)
        {
            bUnsafeMeshSource = true;
            continue;
        }
        bHasAuthoredMeshIdentity = true;
        auto Resolved = PlayerAITryResolveLoadedSoftObject(*SoftMesh, MeshClass);
        if (TestCandidate(Resolved))
            return EPlayerAICharacterPartMeshMatch::Matched;
    }

    if (auto Property = Part->GetProperty("MasterSkeletalMeshes"))
    {
        const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(
                Offsets::Offset_Internal, Offsets::ElementSize)) + sizeof(uint32);
        if (Offsets::ElementSize >= sizeof(int32) &&
            SDK::MemReadable(Property, RequiredMetadataBytes))
        {
            const int32 PropertyOffset = static_cast<int32>(
                SDK::ReadPropertyOffset(GetFromOffset<uint32>(Property, Offsets::Offset_Internal)));
            const uint32 PropertySize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
            const int32 ArrayDimension = GetFromOffset<int32>(Property,
                Offsets::ElementSize - sizeof(int32));
            const int32 OwnerSize = Part->Class->GetPropertiesSize();
            if (PropertyOffset >= 0 && PropertySize == sizeof(TArray<FSoftObjectPtr>) &&
                ArrayDimension == 1 && OwnerSize > PropertyOffset &&
                PropertySize <= static_cast<uint32>(OwnerSize - PropertyOffset))
            {
                auto Meshes = reinterpret_cast<TArray<FSoftObjectPtr>*>(reinterpret_cast<uint8*>(
                            const_cast<UObject*>(Part)) + PropertyOffset);
                const int Count = Meshes->Num();
                const uint32 Stride = FSoftObjectPtr::Size();
                if (Count == 0 && Meshes->Max() >= 0)
                {
                    bInspectedMeshSource = true;
                }
                else if (Count > 0 && Count <= 16 && Meshes->Max() >= Count && Meshes->Data &&
                    Stride >= 0x10 && Stride <= 0x80 && SDK::MemReadable(Meshes->Data,
                        static_cast<size_t>(Count) * Stride))
                {
                    bInspectedMeshSource = true;
                    for (int Index = 0; Index < Count; Index++)
                    {
                        auto& SoftMesh = Meshes->Get(Index, Stride);
                        const auto PathState = PlayerAIGetSoftReferencePathState(SoftMesh);
                        if (PathState == EPlayerAISoftReferencePathState::Empty)
                        {
                            continue;
                        }
                        if (PathState == EPlayerAISoftReferencePathState::Unsafe)
                        {
                            bUnsafeMeshSource = true;
                            continue;
                        }
                        bHasAuthoredMeshIdentity = true;
                        auto Resolved = PlayerAITryResolveLoadedSoftObject(SoftMesh, MeshClass);
                        if (TestCandidate(Resolved))
                        {
                            return EPlayerAICharacterPartMeshMatch::Matched;
                        }
                    }
                }
            }
        }
    }

    return bHasAuthoredMeshIdentity ? EPlayerAICharacterPartMeshMatch::Mismatched
        : (bInspectedMeshSource && !bUnsafeMeshSource ? EPlayerAICharacterPartMeshMatch::Empty
            : EPlayerAICharacterPartMeshMatch::Unavailable);
}

static const UObject* PlayerAITryReadRenderedComponentMesh(UObject* Component,
    const UClass* MeshClass)
{
    if (!PlayerAIIsLiveSupportObject(Component) || !PlayerAIIsLiveSupportObject(MeshClass))
    {
        return nullptr;
    }

    const char* GetterFunctions[] =
    {
        "GetSkeletalMeshAsset", "GetSkinnedAsset",
    };
    for (const auto FunctionName : GetterFunctions)
    {
        bool bFunctionAvailable = false;
        auto Mesh = PlayerAITryCallNoArgObject(Component, FunctionName, MeshClass,
            bFunctionAvailable);
        if (Mesh)
            return Mesh;
    }

    const char* MeshProperties[] =
    {
        "SkeletalMesh", "SkinnedAsset", "SkeletalMeshAsset",
    };
    for (const auto PropertyName : MeshProperties)
    {
        UObject* Mesh = nullptr;
        if (PlayerAITryReadObjectProperty(Component, PropertyName, Mesh) &&
            PlayerAIIsLiveSupportObject(Mesh) && Mesh->IsA(MeshClass))
        {
            return Mesh;
        }
    }
    return nullptr;
}

static bool PlayerAITryGetCheatBotCustomizationReadiness(AFortPlayerPawnAthena* Pawn,
    bool& OutReady)
{
    OutReady = false;
    return PlayerAITryCallNoArgBool(Pawn, "IsCharacterCustomizationLoadingCompleted", OutReady);
}

static bool PlayerAIHasCompletedCheatBotVisualCommit(AFortPlayerPawnAthena* Pawn,
    const UObject* Parts[PlayerAICharacterPartSlotCount], bool bRequireNativeReadiness)
{
    const auto MirrorState = PlayerAIGetCharacterPartMirrorState(nullptr, Pawn, Parts);
    if (bRequireNativeReadiness && MirrorState != EPlayerAICharacterPartMirrorState::Matched)
    {
        return false;
    }

    bool bCustomizationReady = false;
    const bool bReadinessAvailable = PlayerAITryGetCheatBotCustomizationReadiness(
            Pawn, bCustomizationReady);
    if (bRequireNativeReadiness && (!bReadinessAvailable || !bCustomizationReady))
        return false;

    auto MeshClass = PlayerAIResolveCachedClass(PlayerAISkeletalMeshClassCache, "SkeletalMesh");
    if (!PlayerAIIsLiveSupportObject(MeshClass))
        return false;

    bool bVerifiedHeadMesh = false;
    bool bVerifiedBodyMesh = false;
    bool bHeadVisibilityAvailable = false;
    bool bHeadVisible = false;
    const int SlotsToVerify = (std::min)(PlayerAICharacterPartSlotCount, 6);
    for (int Index = 0; Index < SlotsToVerify; Index++)
    {
        bool bGetterAvailable = false;
        auto Component = PlayerAITryGetCharacterPartMeshComponent(Pawn, static_cast<uint8>(Index),
                bGetterAvailable);
        auto RenderedMesh = PlayerAITryReadRenderedComponentMesh(Component, MeshClass);

        if (!Parts[Index])
        {
            if (Index < 2)
                return false;

            if (RenderedMesh)
            {
                bool bVisible = true;
                if (!PlayerAITryCallNoArgBool(Component, "IsVisible", bVisible) || bVisible)
                {
                    return false;
                }
            }
            continue;
        }

        const auto MeshMatch = PlayerAIMatchRenderedCharacterPartMesh(
                Parts[Index], RenderedMesh, MeshClass);
        if (MeshMatch == EPlayerAICharacterPartMeshMatch::Empty)
        {
            if (Index < 2)
                return false;
            if (RenderedMesh)
            {
                bool bVisible = true;
                if (!PlayerAITryCallNoArgBool(Component, "IsVisible", bVisible) || bVisible)
                {
                    return false;
                }
            }
            continue;
        }
        if (MeshMatch != EPlayerAICharacterPartMeshMatch::Matched)
        {
            return false;
        }

        if (!bGetterAvailable || !Component || !RenderedMesh)
            return false;

        if (Index == 0)
        {
            bHeadVisibilityAvailable = PlayerAITryCallNoArgBool(
                    Component, "IsVisible", bHeadVisible);
        }
        else
        {
            bool bVisible = false;
            if (!PlayerAITryCallNoArgBool(Component, "IsVisible", bVisible) || !bVisible)
            {
                return false;
            }
        }
        if (Index == 0)
            bVerifiedHeadMesh = true;
        else if (Index == 1)
            bVerifiedBodyMesh = true;
    }

    return bVerifiedHeadMesh && bVerifiedBodyMesh && bHeadVisibilityAvailable && bHeadVisible &&
        (!bRequireNativeReadiness || MirrorState == EPlayerAICharacterPartMirrorState::Matched);
}

static bool PlayerAITryRepairPawnPlayerStateBinding(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn)
{
    UObject* PawnComponent = nullptr;
    UObject* PlayerStateComponent = nullptr;
    UObject** PawnComponentSlot = nullptr;
    if (!PlayerAITryReadObjectProperty(Pawn, "HACK_CustomPRIComponent",
            PawnComponent, &PawnComponentSlot) || PlayerAIIsLiveSupportObject(PawnComponent) ||
        !PawnComponentSlot || !PlayerAITryReadObjectProperty(PlayerState, "CustomPRIComponent",
            PlayerStateComponent) || !PlayerAIIsLiveSupportObject(PlayerStateComponent))
    {
        return false;
    }

    *PawnComponentSlot = PlayerStateComponent;
    return true;
}

void VersionFeatureAdapter::InitializeSyntheticPawnCosmeticLifecycle(
    AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    if (VersionInfo.FortniteVersion < 19.0 || !PlayerAIIsLiveSupportObject(PlayerState) ||
        !PlayerAIIsLiveSupportObject(Pawn) || Pawn->PlayerState != PlayerState)
    {
        return;
    }

    UObject* PawnComponent = nullptr;
    const bool bHasComponentProperty = PlayerAITryReadObjectProperty(
            Pawn, "HACK_CustomPRIComponent", PawnComponent);
    if (bHasComponentProperty && PlayerAIIsLiveSupportObject(PawnComponent))
    {
        return;
    }

    if (auto OnRepPlayerState = Pawn->GetFunction("OnRep_PlayerState"))
    {
        VersionFeatureAdapter::SafeCallNoArgs(Pawn, OnRepPlayerState);
    }
    PlayerAITryRepairPawnPlayerStateBinding(PlayerState, Pawn);

    UObject* BoundComponent = nullptr;
    if (bHasComponentProperty && (!PlayerAITryReadObjectProperty(
             Pawn, "HACK_CustomPRIComponent", BoundComponent) ||
         !PlayerAIIsLiveSupportObject(BoundComponent)))
    {
        AIDebugLogger::MissingFeature("SyntheticPawnCosmeticLifecycle",
            "the modern pawn cosmetic component remained unavailable; exact native part selection will be used");
    }
}

static bool PlayerAITryServerSetCosmeticLoadout(AFortPlayerControllerAthena* Controller,
    UAthenaCharacterItemDefinition* Character, bool bRefreshPawn)
{
    if (VersionInfo.FortniteVersion < 19.0 || bPlayerAIServerSetCosmeticLoadoutDisabled ||
        !PlayerAIIsLiveSupportObject(Controller) || !PlayerAIIsLiveSupportObject(Character))
    {
        return false;
    }

    uint8* LoadoutData = nullptr;
    uint32 LoadoutSize = 0;
    const UStruct* LoadoutStruct = nullptr;
    uint32 CharacterOffset = 0;
    struct FLoadoutSource
    {
        const char* Name;
        const wchar_t* DirtyName;
    };
    const FLoadoutSource Sources[] =
    {
        { "CosmeticLoadoutBC", L"CosmeticLoadoutBC" },
        { "CustomizationLoadout", L"CustomizationLoadout" },
        { "CosmeticLoadoutPC", L"CosmeticLoadoutPC" },
    };

    int BestSourceScore = -1;
    for (const auto& Source : Sources)
    {
        uint8* CandidateData = nullptr;
        uint32 CandidateSize = 0;
        const UStruct* CandidateStruct = nullptr;
        uint32 CandidateCharacterOffset = 0;
        if (PlayerAIWriteLoadoutCharacter(Controller, Source.Name, Source.DirtyName,
                Character, &CandidateData, &CandidateSize,
                &CandidateStruct, &CandidateCharacterOffset, false))
        {
            int CandidateScore = 0;
            const char* ImportantSlots[] =
            {
                "Pickaxe", "Backpack", "Glider", "SkyDiveContrail", "CharmOverride",
            };
            for (const auto PropertyName : ImportantSlots)
            {
                auto Property = CandidateStruct->GetProperty(PropertyName, 0x10000);
                if (!Property || Offsets::ElementSize < sizeof(int32))
                {
                    continue;
                }

                const int32 Offset = static_cast<int32>(
                    SDK::ReadPropertyOffset(GetFromOffset<uint32>(
                        Property, Offsets::Offset_Internal)));
                const uint32 Size = GetFromOffset<uint32>(Property, Offsets::ElementSize);
                if (Offset < 0 || Size != sizeof(UObject*) ||
                    static_cast<uint32>(Offset) > CandidateSize || sizeof(UObject*) >
                        CandidateSize - static_cast<uint32>(Offset))
                {
                    continue;
                }

                UObject* Value = nullptr;
                memcpy(&Value, CandidateData + Offset, sizeof(Value));
                if (PlayerAIIsLiveSupportObject(Value))
                {
                    CandidateScore += strcmp(PropertyName, "Pickaxe") == 0 ? 100 : 1;
                }
            }

            if (CandidateScore > BestSourceScore)
            {
                BestSourceScore = CandidateScore;
                LoadoutData = CandidateData;
                LoadoutSize = CandidateSize;
                LoadoutStruct = CandidateStruct;
                CharacterOffset = CandidateCharacterOffset;
            }
        }
    }

    auto Function = Controller->GetFunction("ServerSetCosmeticLoadout");
    if (!LoadoutData || LoadoutSize == 0 || LoadoutSize > 0x1000 ||
        !PlayerAIIsLiveSupportObject(Function))
    {
        if (LoadoutData && LoadoutSize > 0 && !PlayerAIIsLiveSupportObject(Function))
        {
            bPlayerAIServerSetCosmeticLoadoutDisabled = true;
        }
        return false;
    }

    const auto Params = Function->GetParamsNamed();
    if (Params.Size == 0 || Params.Size > 0x1000)
    {
        bPlayerAIServerSetCosmeticLoadoutDisabled = true;
        AIDebugLogger::MissingFeature("ServerSetCosmeticLoadoutForPlayerAI",
            "the native loadout parameter buffer was invalid; exact character parts remain authoritative");
        return false;
    }

    const UFunction::ParamNamed* LoadoutParam = nullptr;
    const UFunction::ParamNamed* RefreshParam = nullptr;
    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "Loadout")
            LoadoutParam = &Param;
        else if (Param.Name == "bRefreshPawn")
            RefreshParam = &Param;
    }

    constexpr uint64 CPF_Parm = 0x80;
    constexpr uint64 CPF_OutParm = 0x100;
    constexpr uint64 CPF_ReturnParm = 0x400;
    constexpr uint64 CPF_ReferenceParm = 0x8000000;
    auto IsInput = [=](const UFunction::ParamNamed* Param, uint32 ExpectedSize)
        {
            return Param && Param->ElementSize == ExpectedSize &&
                (Param->PropertyFlags & CPF_Parm) && !(Param->PropertyFlags &
                    (CPF_OutParm | CPF_ReturnParm));
        };
    auto IsLoadoutReference = [=](const UFunction::ParamNamed* Param, uint32 ExpectedSize)
        {
            return Param && Param->ElementSize == ExpectedSize &&
                (Param->PropertyFlags & CPF_Parm) && (Param->PropertyFlags & CPF_OutParm) &&
                (Param->PropertyFlags & CPF_ReferenceParm) &&
                !(Param->PropertyFlags & CPF_ReturnParm);
        };
    auto RangesOverlap = [](uint32 FirstOffset, uint32 FirstSize,
        uint32 SecondOffset, uint32 SecondSize)
        {
            return FirstOffset < SecondOffset + SecondSize &&
                SecondOffset < FirstOffset + FirstSize;
        };
    const bool bValidLayout = LoadoutParam && RefreshParam && Params.NameOffsetMap.size() == 2 &&
        Function->GetPropertiesSize() == Params.Size &&
        IsLoadoutReference(LoadoutParam, LoadoutSize) && IsInput(RefreshParam, sizeof(bool)) &&
        CharacterOffset <= LoadoutSize && sizeof(Character) <= LoadoutSize - CharacterOffset &&
        LoadoutParam->Offset <= Params.Size && LoadoutSize <= Params.Size - LoadoutParam->Offset &&
        RefreshParam->Offset < Params.Size && !RangesOverlap(LoadoutParam->Offset, LoadoutSize,
            RefreshParam->Offset, sizeof(bool));
    if (!bValidLayout)
    {
        bPlayerAIServerSetCosmeticLoadoutDisabled = true;
        AIDebugLogger::MissingFeature("ServerSetCosmeticLoadoutForPlayerAI",
            "the reflected loadout layout did not match this build; exact character parts remain authoritative");
        return false;
    }

    std::vector<uint8> Buffer(Params.Size, 0);
    auto ParamLoadout = Buffer.data() + LoadoutParam->Offset;
    // A raw FortAthenaLoadout copy is unsafe: some versions hold owning arrays and there is no deep-copy API.
    const char* PreservedObjectProperties[] =
    {
        "Backpack", "Pickaxe", "Glider", "SkyDiveContrail", "Contrail", "LoadingScreen",
        "MusicPack", "VictoryPose", "PetSkin", "Hat", "BattleBus", "VehicleDecoration",
        "CallingCard", "MapMarker", "ItemWrapOverride", "CharmOverride", "Charm",
    };
    for (const auto PropertyName : PreservedObjectProperties)
    {
        auto Property = LoadoutStruct->GetProperty(PropertyName, 0x10000);
        if (!Property)
            continue;

        const size_t RequiredMetadataBytes = static_cast<size_t>((std::max)(
                Offsets::Offset_Internal, Offsets::ElementSize)) + sizeof(uint32);
        if (!SDK::MemReadable(Property, RequiredMetadataBytes))
        {
            continue;
        }

        const int32 PropertyOffset = static_cast<int32>(
            SDK::ReadPropertyOffset(GetFromOffset<uint32>(Property, Offsets::Offset_Internal)));
        const uint32 PropertySize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
        const int32 ArrayDimension = GetFromOffset<int32>(Property,
            Offsets::ElementSize - sizeof(int32));
        if (PropertyOffset < 0 || PropertySize != sizeof(UObject*) || ArrayDimension != 1 ||
            static_cast<uint32>(PropertyOffset) > LoadoutSize || sizeof(UObject*) >
                LoadoutSize - static_cast<uint32>(PropertyOffset))
        {
            continue;
        }

        UObject* Value = nullptr;
        auto Source = LoadoutData + PropertyOffset;
        if (!SDK::MemReadable(Source, sizeof(Value)))
            continue;
        memcpy(&Value, Source, sizeof(Value));
        if (Value && !PlayerAIIsLiveSupportObject(Value))
            continue;
        memcpy(ParamLoadout + PropertyOffset, &Value, sizeof(Value));
    }

    memcpy(ParamLoadout + CharacterOffset, &Character, sizeof(Character));
    PlayerAIWriteStructBool(ParamLoadout, LoadoutStruct, LoadoutSize, "bIsDefaultCharacter", false);
    PlayerAIWriteStructBool(ParamLoadout, LoadoutStruct, LoadoutSize, "bForceUpdateVariants", true);
    Buffer[RefreshParam->Offset] = bRefreshPawn ? 1 : 0;

    if (!PlayerAIGuardedProcessEvent(Controller, Function, Buffer.data()))
    {
        bPlayerAIServerSetCosmeticLoadoutDisabled = true;
        AIDebugLogger::MissingFeature("ServerSetCosmeticLoadoutForPlayerAI",
            "the guarded native loadout initialization faulted; exact character parts remain authoritative");
        return false;
    }

    static bool bReportedNativeLoadoutCommit[2]{};
    const int RefreshIndex = bRefreshPawn ? 1 : 0;
    if (!bReportedNativeLoadoutCommit[RefreshIndex])
    {
        bReportedNativeLoadoutCommit[RefreshIndex] = true;
        AIDebugLogger::Log("Cosmetics", bRefreshPawn
                ? "cheat-bot native loadout setter invoked with one pawn refresh"
                : "cheat-bot native loadout setter invoked for initialization without a pawn refresh");
    }
    return true;
}

static bool PlayerAITryApplyCharacterCosmetics(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, const UObject* Parts[PlayerAICharacterPartSlotCount],
    bool* OutInvoked)
{
    if (OutInvoked)
        *OutInvoked = false;

    if (VersionInfo.FortniteVersion < 19.0 || bPlayerAIApplyCharacterCosmeticsDisabled ||
        !PlayerAIIsLiveSupportObject(PlayerState) || !PlayerAIIsLiveSupportActor(Pawn) || !Parts)
    {
        return false;
    }

    auto Library = UFortKismetLibrary::GetDefaultObj();
    auto Function = Library ? Library->GetFunction("ApplyCharacterCosmetics") : nullptr;
    if (!PlayerAIIsLiveSupportObject(Library) || !PlayerAIIsLiveSupportObject(Function))
    {
        return false;
    }

    const auto Params = Function->GetParamsNamed();
    if (Params.Size == 0 || Params.Size > 0x1000)
    {
        bPlayerAIApplyCharacterCosmeticsDisabled = true;
        return false;
    }

    const UFunction::ParamNamed* WorldParam = nullptr;
    const UFunction::ParamNamed* PartsParam = nullptr;
    const UFunction::ParamNamed* PlayerStateParam = nullptr;
    const UFunction::ParamNamed* SuccessParam = nullptr;
    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "WorldContextObject")
            WorldParam = &Param;
        else if (Param.Name == "CharacterParts")
            PartsParam = &Param;
        else if (Param.Name == "PlayerState")
            PlayerStateParam = &Param;
        else if (Param.Name == "bSuccess")
            SuccessParam = &Param;
    }

    constexpr uint64 CPF_Parm = 0x80;
    constexpr uint64 CPF_OutParm = 0x100;
    constexpr uint64 CPF_ReturnParm = 0x400;
    auto IsInput = [=](const UFunction::ParamNamed* Param, uint32 ExpectedSize,
        bool bAllowOutFlag = false)
        {
            return Param && Param->ElementSize == ExpectedSize &&
                (Param->PropertyFlags & CPF_Parm) && !(Param->PropertyFlags & CPF_ReturnParm) &&
                (bAllowOutFlag || !(Param->PropertyFlags & CPF_OutParm));
        };
    auto IsOutput = [=](const UFunction::ParamNamed* Param, uint32 ExpectedSize)
        {
            return Param && Param->ElementSize == ExpectedSize &&
                (Param->PropertyFlags & CPF_Parm) && (Param->PropertyFlags & CPF_OutParm) &&
                !(Param->PropertyFlags & CPF_ReturnParm);
        };
    auto RangesOverlap = [](uint32 FirstOffset, uint32 FirstSize,
        uint32 SecondOffset, uint32 SecondSize)
        {
            return FirstOffset < SecondOffset + SecondSize &&
                SecondOffset < FirstOffset + FirstSize;
        };
    const bool bValidLayout = WorldParam && PartsParam && PlayerStateParam && SuccessParam &&
        Params.NameOffsetMap.size() == 4 && Function->GetPropertiesSize() == Params.Size &&
        IsInput(WorldParam, sizeof(UObject*)) && IsInput(PartsParam, sizeof(TArray<UObject*>)) &&
        IsInput(PlayerStateParam, sizeof(UObject*)) && IsOutput(SuccessParam, sizeof(bool)) &&
        WorldParam->Offset <= Params.Size && sizeof(UObject*) <= Params.Size - WorldParam->Offset &&
        PartsParam->Offset <= Params.Size &&
        sizeof(TArray<UObject*>) <= Params.Size - PartsParam->Offset &&
        PlayerStateParam->Offset <= Params.Size &&
        sizeof(UObject*) <= Params.Size - PlayerStateParam->Offset &&
        SuccessParam->Offset < Params.Size && !RangesOverlap(WorldParam->Offset, sizeof(UObject*),
            PartsParam->Offset, sizeof(TArray<UObject*>)) && !RangesOverlap(
            WorldParam->Offset, sizeof(UObject*), PlayerStateParam->Offset, sizeof(UObject*)) &&
        !RangesOverlap(WorldParam->Offset, sizeof(UObject*), SuccessParam->Offset, sizeof(bool)) &&
        !RangesOverlap(PartsParam->Offset, sizeof(TArray<UObject*>),
            PlayerStateParam->Offset, sizeof(UObject*)) && !RangesOverlap(
            PartsParam->Offset, sizeof(TArray<UObject*>), SuccessParam->Offset, sizeof(bool)) &&
        !RangesOverlap(PlayerStateParam->Offset, sizeof(UObject*),
            SuccessParam->Offset, sizeof(bool));
    if (!bValidLayout)
    {
        bPlayerAIApplyCharacterCosmeticsDisabled = true;
        AIDebugLogger::MissingFeature("ApplyCharacterCosmeticsForPlayerAI",
            "the reflected Kismet cosmetic layout did not match this build");
        return false;
    }

    UObject* CompactParts[PlayerAICharacterPartSlotCount]{};
    int32 CompactPartCount = 0;
    for (int Index = 0;
         Index < PlayerAICharacterPartSlotCount; Index++)
    {
        if (PlayerAIIsLiveSupportObject(Parts[Index]))
        {
            CompactParts[CompactPartCount++] = const_cast<UObject*>(Parts[Index]);
        }
    }
    if (CompactPartCount == 0)
        return false;

    TArray<UObject*> CharacterParts;
    CharacterParts.Data = CompactParts;
    CharacterParts.NumElements = CompactPartCount;
    CharacterParts.MaxElements = CompactPartCount;

    std::vector<uint8> Buffer(Params.Size, 0);
    UObject* WorldContext = UWorld::GetWorld();
    UObject* MutablePlayerState = PlayerState;
    memcpy(Buffer.data() + WorldParam->Offset, &WorldContext, sizeof(WorldContext));
    memcpy(Buffer.data() + PartsParam->Offset, &CharacterParts, sizeof(CharacterParts));
    memcpy(Buffer.data() + PlayerStateParam->Offset,
        &MutablePlayerState, sizeof(MutablePlayerState));

    if (OutInvoked)
        *OutInvoked = true;
    if (!PlayerAIGuardedProcessEvent(Library, Function, Buffer.data()))
    {
        bPlayerAIApplyCharacterCosmeticsDisabled = true;
        AIDebugLogger::MissingFeature("ApplyCharacterCosmeticsForPlayerAI",
            "the guarded Kismet cosmetic commit faulted");
        return false;
    }

    return Buffer[SuccessParam->Offset] != 0;
}

static bool PlayerAIFinishCosmetics(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, const UObject* Parts[PlayerAICharacterPartSlotCount],
    UAthenaCharacterItemDefinition* Character = nullptr)
{
    // Synthetic PlayerStates start zeroed, which leaves the Chapter 1 head-accessory preference off.
    const bool bWroteHeadAccessoryPreference = PlayerAISetReflectedReadyBool(
            PlayerState, "bShowHeroHeadAccessories", true);
    if (bWroteHeadAccessoryPreference)
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(
            PlayerState, L"bShowHeroHeadAccessories");
    }
    const bool bWroteBackpackPreference = PlayerAISetReflectedReadyBool(
            PlayerState, "bShowHeroBackpack", true);
    if (bWroteBackpackPreference)
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(PlayerState, L"bShowHeroBackpack");
    }

    AFortPlayerControllerAthena* CheatBotController = nullptr;
    if (Pawn && Pawn->HasController() && PlayerAIIsLiveSupportObject(Pawn->Controller))
    {
        CheatBotController = Pawn->Controller->Cast<AFortPlayerControllerAthena>();
        if (!AFortPlayerControllerAthena::IsCheatSpawnedBotController(CheatBotController))
        {
            CheatBotController = nullptr;
        }
    }

    const bool bUseConfirmedCheatBotPath = VersionInfo.FortniteVersion >= 19.0 &&
        CheatBotController;
    if (Character && !bUseConfirmedCheatBotPath)
        PlayerAIWriteCharacterLoadout(Pawn, Character);

    if (bUseConfirmedCheatBotPath)
    {
        PlayerAITryRepairPawnPlayerStateBinding(PlayerState, Pawn);

        const FPlayerAICharacterLoadoutSnapshot
            PreviousLoadouts[] =
            {
                PlayerAICaptureCharacterLoadout(Pawn, "CosmeticLoadout", L"CosmeticLoadout"),
                PlayerAICaptureCharacterLoadout(
                    Pawn, "BaseCosmeticLoadout", L"BaseCosmeticLoadout"),
                PlayerAICaptureCharacterLoadout(
                    Pawn, "AppliedCosmeticLoadout", L"AppliedCosmeticLoadout"),
                PlayerAICaptureCharacterLoadout(
                    CheatBotController, "CosmeticLoadoutPC", L"CosmeticLoadoutPC"),
                PlayerAICaptureCharacterLoadout(
                    CheatBotController, "CustomizationLoadout", L"CustomizationLoadout"),
                PlayerAICaptureCharacterLoadout(
                    CheatBotController, "CosmeticLoadoutBC", L"CosmeticLoadoutBC"),
                };

        const bool bSetterInvoked = Character && PlayerAITryServerSetCosmeticLoadout(
                CheatBotController, Character, false);

        bool bApplyInvoked = false;
        const bool bApplySucceeded = PlayerAITryApplyCharacterCosmetics(PlayerState, Pawn, Parts,
                &bApplyInvoked);
        bool bRefreshInvoked = false;
        bool bExactPartsInvoked = false;
        bool bVisualCommitSucceeded = bApplySucceeded;
        if (!bVisualCommitSucceeded && Character)
        {
            bRefreshInvoked = PlayerAITryServerSetCosmeticLoadout(
                    CheatBotController, Character, true);
            if (bRefreshInvoked)
            {
                const auto NativeMirrorState = PlayerAIGetCharacterPartMirrorState(
                        nullptr, Pawn, Parts);
                bVisualCommitSucceeded = NativeMirrorState ==
                        EPlayerAICharacterPartMirrorState::Matched;
            }
        }

        if (!bVisualCommitSucceeded && !bRefreshInvoked)
        {
            bExactPartsInvoked = PlayerAIChoosePartsOnPawn(Pawn, Parts);
            if (bExactPartsInvoked)
            {
                const auto NativeMirrorState = PlayerAIGetCharacterPartMirrorState(
                        nullptr, Pawn, Parts);
                bVisualCommitSucceeded = NativeMirrorState ==
                        EPlayerAICharacterPartMirrorState::Matched;
            }
        }

        if (!bVisualCommitSucceeded)
        {
            for (const auto& PreviousLoadout : PreviousLoadouts)
                PlayerAIRestoreCharacterLoadout(PreviousLoadout);
            AIDebugLogger::MissingFeature("BoundedCheatBotCosmeticFallback", bApplyInvoked
                    ? "the native cosmetic call rejected the synthetic bot and its one-shot loadout refresh did not update the pawn"
                    : "no validated one-shot visual commit path was available for the synthetic bot");
            return false;
        }

        if (Character)
            PlayerAIWriteCharacterLoadout(Pawn, Character);
        if (!PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts))
        {
            return false;
        }
        PlayerAIWriteLocalCharacterParts(PlayerState, Parts);
        PlayerAIWritePawnCharacterParts(Pawn, Parts);

        PlayerState->FlushNetDormancy();
        PlayerState->ForceNetUpdate();
        Pawn->FlushNetDormancy();
        Pawn->ForceNetUpdate();
        CheatBotController->FlushNetDormancy();
        CheatBotController->ForceNetUpdate();

        static bool bReportedConfirmedCheatBotCommit = false;
        if (!bReportedConfirmedCheatBotCommit)
        {
            bReportedConfirmedCheatBotCommit = true;
            AIDebugLogger::Log("Cosmetics", bApplySucceeded ? (bSetterInvoked
                        ? "cheat-bot outfit applied after initializing its native loadout"
                        : "cheat-bot outfit applied through the native cosmetic path")
                    : (bRefreshInvoked
                        ? "cheat-bot outfit applied through one native loadout refresh"
                        : "cheat-bot outfit applied through exact native character parts"));
        }
        return true;
    }

    if (VersionInfo.FortniteVersion >= 23.0)
    {
        if (!PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts))
        {
            return false;
        }

        if (Character && Pawn)
        {
            const char* LoadoutNotifications[] =
            {
                "OnRep_BaseCosmeticLoadout", "OnRep_CosmeticLoadout",
            };
            for (auto Name : LoadoutNotifications)
            {
                if (auto Notification = Pawn->GetFunction(Name))
                {
                    VersionFeatureAdapter::SafeCallNoArgs(Pawn, Notification);
                    break;
                }
            }
        }

        PlayerAITryUpdateCharacterPartsVisualization(PlayerState);
        PlayerAIChoosePartsOnPawn(Pawn, Parts);
        PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts);
        PlayerAIWriteLocalCharacterParts(PlayerState, Parts);
        PlayerAIWritePawnCharacterParts(Pawn, Parts);

        if (Pawn)
        {
            if (auto PartsReinitialized = Pawn->GetFunction("OnCharacterPartsReinitialized"))
            {
                VersionFeatureAdapter::SafeCallNoArgs(Pawn, PartsReinitialized);
            }
        }

        PlayerState->FlushNetDormancy();
        PlayerState->ForceNetUpdate();
        if (Pawn)
        {
            Pawn->FlushNetDormancy();
            Pawn->ForceNetUpdate();
        }
        return true;
    }

    // Older versions retain their established native/repnotify ordering.
    PlayerAIChoosePartsOnPawn(Pawn, Parts);
    if (!PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts))
    {
        return false;
    }

    const bool bVisualizerInvoked = PlayerAITryUpdateCharacterPartsVisualization(PlayerState);
    if (bVisualizerInvoked)
    {
        auto MirrorState = PlayerAIGetCharacterPartMirrorState(PlayerState, Pawn, Parts);

        const bool bNeedsBoundedVisualizerFallback = MirrorState ==
                EPlayerAICharacterPartMirrorState::Mismatched ||
            (VersionInfo.FortniteVersion >= 19.0 && MirrorState ==
                EPlayerAICharacterPartMirrorState::Unavailable);
        if (bNeedsBoundedVisualizerFallback)
        {
            static bool bReportedVisualizerFallback = false;
            if (!bReportedVisualizerFallback)
            {
                bReportedVisualizerFallback = true;
                AIDebugLogger::Log("Cosmetics",
                    "the modern visualizer did not commit the requested parts; using the bounded repnotify fallback");
            }

            if (Character && Pawn)
            {
                if (auto LoadoutNotification = Pawn->GetFunction("OnRep_CosmeticLoadout"))
                {
                    VersionFeatureAdapter::SafeCallNoArgs(Pawn, LoadoutNotification);
                }
            }

            const char* PartNotifications[] =
            {
                "OnRep_CharacterData", "OnRep_CharacterParts",
            };
            for (auto Name : PartNotifications)
            {
                if (auto Notification = PlayerState->GetFunction(Name))
                {
                    VersionFeatureAdapter::SafeCallNoArgs(PlayerState, Notification);
                    break;
                }
            }

            MirrorState = PlayerAIGetCharacterPartMirrorState(PlayerState, Pawn, Parts);
            if (MirrorState == EPlayerAICharacterPartMirrorState::Mismatched ||
                (VersionInfo.FortniteVersion >= 19.0 && MirrorState ==
                    EPlayerAICharacterPartMirrorState::Unavailable))
            {
                PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts);
                PlayerAIWriteLocalCharacterParts(PlayerState, Parts);
                PlayerAIWritePawnCharacterParts(Pawn, Parts);

                if (Pawn)
                {
                    if (auto Reinitialized = Pawn->GetFunction("OnCharacterPartsReinitialized"))
                    {
                        VersionFeatureAdapter::SafeCallNoArgs(Pawn, Reinitialized);
                    }
                }
            }
        }

        PlayerState->FlushNetDormancy();
        PlayerState->ForceNetUpdate();

        if (Pawn)
        {
            Pawn->FlushNetDormancy();
            Pawn->ForceNetUpdate();
        }

        return true;
    }

    PlayerAIChooseGenderOnPawn(Pawn, Character && Character->HasGender() ? Character->Gender
            : EFortCustomGender::Female);

    const char* InitializationNotifications[] =
    {
        "OnRep_HeroType", "OnRep_CharacterGender", "OnRep_CharacterBodyType", "OnRep_Gender",
    };

    for (auto Name : InitializationNotifications)
    {
        if (auto Notification = PlayerState->GetFunction(Name))
            VersionFeatureAdapter::SafeCallNoArgs(PlayerState, Notification);
    }

    static bool bNativeCustomizationDisabled = false;
    const bool bFortnite250 = VersionInfo.FortniteVersion >= 2.49 &&
        VersionInfo.FortniteVersion < 2.51;

    if (bFortnite250 && Pawn)
    {
        // 2.50's profile-driven customization overwrites directly selected parts on a synthetic PlayerState.
        if (auto OnRepPlayerState = Pawn->GetFunction("OnRep_PlayerState"))
        {
            VersionFeatureAdapter::SafeCallNoArgs(Pawn, OnRepPlayerState);
        }
    }
    else if (ApplyCharacterCustomization && Pawn && !bNativeCustomizationDisabled)
    {
        if (!PlayerAITryNativeCustomization(ApplyCharacterCustomization, PlayerState, Pawn))
        {
            bNativeCustomizationDisabled = true;
            AIDebugLogger::MissingFeature("NativeCharacterCustomizationForPlayerAI",
                "native customization call faulted and was disabled - parts replicate via the player state");
        }
    }

    PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts);

    if (Character)
    {
        PlayerAIWriteCharacterLoadout(Pawn, Character);

        if (Pawn)
        {
            if (auto LoadoutNotification = Pawn->GetFunction("OnRep_CosmeticLoadout"))
            {
                VersionFeatureAdapter::SafeCallNoArgs(Pawn, LoadoutNotification);
            }
        }
    }

    if (Pawn && VersionInfo.FortniteVersion < 9.0)
    {
        if (auto PlayerStateNotification = Pawn->GetFunction("OnRep_PlayerState"))
        {
            VersionFeatureAdapter::SafeCallNoArgs(Pawn, PlayerStateNotification);
            PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts);
        }
    }

    const char* PartNotifications[] =
    {
        "OnRep_CharacterData", "OnRep_CharacterParts",
    };

    for (auto Name : PartNotifications)
    {
        if (auto Notification = PlayerState->GetFunction(Name))
            VersionFeatureAdapter::SafeCallNoArgs(PlayerState, Notification);
    }

    if (bWroteHeadAccessoryPreference)
    {
        if (auto Notification = PlayerState->GetFunction("OnRep_ShowHeroHeadAccessories"))
        {
            VersionFeatureAdapter::SafeCallNoArgs(PlayerState, Notification);
        }
    }
    if (bWroteBackpackPreference)
    {
        if (auto Notification = PlayerState->GetFunction("OnRep_ShowHeroBackpack"))
        {
            VersionFeatureAdapter::SafeCallNoArgs(PlayerState, Notification);
        }
    }

    PlayerAIWriteLocalCharacterParts(PlayerState, Parts);
    PlayerAIWritePawnCharacterParts(Pawn, Parts);

    if (Pawn)
    {
        if (auto PartsReinitialized = Pawn->GetFunction("OnCharacterPartsReinitialized"))
        {
            VersionFeatureAdapter::SafeCallNoArgs(Pawn, PartsReinitialized);
        }
    }

    PlayerState->FlushNetDormancy();
    PlayerState->ForceNetUpdate();

    if (Pawn)
    {
        Pawn->FlushNetDormancy();
        Pawn->ForceNetUpdate();
    }
    return true;
}

static bool PlayerAIFinishRequestedCheatBotCosmetics(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, const UObject* Parts[PlayerAICharacterPartSlotCount],
    UAthenaCharacterItemDefinition* Character, const FPlayerAICosmeticMetadataSnapshot*
        PreviousMetadata = nullptr, bool* OutTargetRetained = nullptr)
{
    if (OutTargetRetained)
        *OutTargetRetained = false;
    const bool bNeedsDeferredCheatBotPath = VersionInfo.FortniteVersion >= 19.0 &&
        Character && Pawn && PendingBotSkinCommitPawnCounts.contains(Pawn);
    if (!bNeedsDeferredCheatBotPath)
    {
        return PlayerAIFinishCosmetics(PlayerState, Pawn, Parts, Character);
    }

    AFortPlayerControllerAthena* Controller = nullptr;
    if (Pawn->HasController() && PlayerAIIsLiveSupportObject(Pawn->Controller))
    {
        Controller = Pawn->Controller->Cast<AFortPlayerControllerAthena>();
    }
    if (!Controller || !AFortPlayerControllerAthena::IsCheatSpawnedBotController(Controller))
    {
        if (PendingBotSkinCommitPawnCounts.contains(Pawn))
            return false;
        return PlayerAIFinishCosmetics(PlayerState, Pawn, Parts, Character);
    }

    constexpr ULONGLONG NativeRefreshWaitMs = 750ULL;
    constexpr ULONGLONG NativeRefreshHardWaitMs = 3000ULL;
    constexpr ULONGLONG ExactPartsWaitMs = 500ULL;
    constexpr ULONGLONG StableVisualEvidenceMs = 50ULL;
    constexpr ULONGLONG MaximumVisualCommitMs = 15000ULL;
    const ULONGLONG Now = GetTickCount64();

    auto RecoverPendingState = [&](FPlayerAIPendingCheatBotVisualCommit& Pending)
            -> EPlayerAICosmeticRecoveryResult
        {
            const auto Recovery = PlayerAIRestorePendingCheatBotVisualCommit(Pending);
            if (Recovery == EPlayerAICosmeticRecoveryResult::TargetRetained && OutTargetRetained)
            {
                *OutTargetRetained = true;
            }
            return Recovery;
        };

    auto PublishAuthoritativeInputs = [&](bool bWriteControllerLoadouts) -> bool
        {
            if (!PlayerAIWriteReplicatedCharacterParts(PlayerState, Parts))
            {
                return false;
            }

            PlayerAIWriteCharacterLoadout(Pawn, Character, bWriteControllerLoadouts);

            if (Character->HasHeroDefinition() && PlayerAIIsLiveSupportObject(
                    reinterpret_cast<const UObject*>(Character->HeroDefinition)))
            {
                PlayerAIWriteHeroType(PlayerState, reinterpret_cast<const UObject*>(
                        Character->HeroDefinition));
            }
            EFortCustomGender ConfirmedGender = EFortCustomGender::Invalid;
            if (PlayerAIResolveConcreteGender(PlayerState, Character, Parts, ConfirmedGender))
            {
                PlayerAIWriteGender(PlayerState, ConfirmedGender, true);
            }
            uint8 ConfirmedBodyType = 0;
            if (PlayerAIResolveConcreteBodyType(PlayerState, Parts, ConfirmedBodyType))
            {
                PlayerAIWriteBodyType(PlayerState, ConfirmedBodyType, true);
            }
            if (PlayerAISetReflectedReadyBool(PlayerState, "bShowHeroHeadAccessories", true))
            {
                VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                    PlayerState, L"bShowHeroHeadAccessories");
            }
            if (PlayerAISetReflectedReadyBool(PlayerState, "bShowHeroBackpack", true))
            {
                VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                    PlayerState, L"bShowHeroBackpack");
            }

            PlayerState->FlushNetDormancy();
            PlayerState->ForceNetUpdate();
            Pawn->FlushNetDormancy();
            Pawn->ForceNetUpdate();
            Controller->FlushNetDormancy();
            Controller->ForceNetUpdate();
            return true;
        };

    auto PublishConfirmedParts = [&]() -> bool
        {
            if (!PublishAuthoritativeInputs(true))
                return false;

            PlayerAIWriteLocalCharacterParts(PlayerState, Parts);
            PlayerAIWritePawnCharacterParts(Pawn, Parts);
            PlayerState->ForceNetUpdate();
            Pawn->ForceNetUpdate();
            return true;
        };

    auto Existing = PendingCheatBotVisualCommits.find(Pawn);
    if (Existing != PendingCheatBotVisualCommits.end())
    {
        auto& Pending = Existing->second;
        bool bExpectedParts = true;
        for (int Index = 0;
             Index < PlayerAICharacterPartSlotCount; Index++)
        {
            if (Pending.Parts[Index].Get() != Parts[Index])
            {
                bExpectedParts = false;
                break;
            }
        }

        const bool bValidPending = Pending.World.Get() == UWorld::GetWorld() &&
            Pending.Controller.Get() == Controller && Pending.PlayerState.Get() == PlayerState &&
            Pending.Pawn.Get() == Pawn && Pending.Character.Get() == Character &&
            Controller->Pawn == Pawn && Controller->PlayerState == PlayerState &&
            Pawn->HasController() && Pawn->Controller == Controller && bExpectedParts &&
            Now - Pending.QueuedAt < MaximumVisualCommitMs;
        if (!bValidPending)
        {
            RecoverPendingState(Pending);
            PendingCheatBotVisualCommits.erase(Existing);
            return false;
        }

        if (Now < Pending.RetryAt)
            return false;

        if (Pending.Phase == EPlayerAICheatBotVisualCommitPhase::DispatchExactParts)
        {
            if (!PlayerAIVisualProofBudgetAvailable)
            {
                Pending.RetryAt = Now + 1ULL;
                return false;
            }
            PlayerAIVisualProofBudgetAvailable = false;

            bool bAllPartsDispatched = false;
            if (PlayerAIChoosePartsOnPawn(Pawn, Parts, &Pending.ExactPartCursor,
                    &bAllPartsDispatched))
            {
                bPlayerAILastCheatBotVisualCallPerformedNativeWork = true;
                if (bAllPartsDispatched)
                {
                    Pending.Phase = EPlayerAICheatBotVisualCommitPhase::AwaitExactParts;
                    Pending.RetryAt = Now + ExactPartsWaitMs;
                }
                else
                {
                    Pending.RetryAt = Now + 1ULL;
                }
                return false;
            }

            Pending.Phase = EPlayerAICheatBotVisualCommitPhase::AwaitNativeRefresh;
            Pending.RetryAt = Now + 100ULL;
            return false;
        }

        if (!PlayerAIVisualProofBudgetAvailable)
        {
            Pending.RetryAt = Now + 1ULL;
            return false;
        }
        PlayerAIVisualProofBudgetAvailable = false;

        const bool bStrongVisualEvidence = PlayerAIHasCompletedCheatBotVisualCommit(Pawn, Parts,
                Pending.Phase == EPlayerAICheatBotVisualCommitPhase::AwaitNativeRefresh);
        if (bStrongVisualEvidence)
        {
            if (!Pending.MirrorMatchedAt)
            {
                Pending.MirrorMatchedAt = Now;
                Pending.RetryAt = Now + StableVisualEvidenceMs;
                return false;
            }
            if (Now - Pending.MirrorMatchedAt <StableVisualEvidenceMs)
            {
                Pending.RetryAt = Pending.MirrorMatchedAt + StableVisualEvidenceMs;
                return false;
            }

            auto CompletedTransaction = Pending;
            const bool bUsedRefresh = Pending.Phase ==
                    EPlayerAICheatBotVisualCommitPhase::AwaitNativeRefresh;
            PendingCheatBotVisualCommits.erase(Existing);
            if (!PublishConfirmedParts())
            {
                RecoverPendingState(CompletedTransaction);
                return false;
            }

            static bool bReportedDeferredCommit[2]{};
            const int CommitIndex = bUsedRefresh ? 0 : 1;
            if (!bReportedDeferredCommit[CommitIndex])
            {
                bReportedDeferredCommit[CommitIndex] = true;
                AIDebugLogger::Log("Cosmetics", bUsedRefresh
                        ? "cheat-bot outfit verified after one deferred native loadout refresh"
                        : "cheat-bot outfit verified after one exact-part fallback");
            }
            return true;
        }

        Pending.MirrorMatchedAt = 0;
        if (Pending.Phase == EPlayerAICheatBotVisualCommitPhase::AwaitNativeRefresh)
        {
            const ULONGLONG NativeStartedAt = Pending.NativeRefreshStartedAt
                ? Pending.NativeRefreshStartedAt : Pending.QueuedAt;
            const ULONGLONG NativeQuietWindowMs = Pending.bApplyCosmeticsRejected &&
                    !Pending.bNativeLoadoutSetterAttempted ? NativeRefreshWaitMs
                : NativeRefreshHardWaitMs;
            if (Now - NativeStartedAt <NativeQuietWindowMs)
            {
                Pending.RetryAt = Now + 100ULL;
                return false;
            }

            if (!Pending.bNativeLoadoutSetterAttempted)
            {
                Pending.bNativeLoadoutSetterAttempted = true;
                if (PlayerAITryServerSetCosmeticLoadout(Controller, Character, true))
                {
                    bPlayerAILastCheatBotVisualCallPerformedNativeWork = true;
                    Pending.bApplyCosmeticsRejected = false;
                    Pending.NativeRefreshStartedAt = Now;
                    Pending.RetryAt = Now + NativeRefreshWaitMs;
                    return false;
                }

                Pending.Phase = EPlayerAICheatBotVisualCommitPhase::DispatchExactParts;
                Pending.ExactPartCursor = 0;
                Pending.RetryAt = Now + 100ULL;
                return false;
            }

            Pending.Phase = EPlayerAICheatBotVisualCommitPhase::DispatchExactParts;
            Pending.ExactPartCursor = 0;
            Pending.RetryAt = Now + 1ULL;
            return false;
        }

        if (Pending.Phase == EPlayerAICheatBotVisualCommitPhase::AwaitExactParts &&
            Now - Pending.QueuedAt < MaximumVisualCommitMs)
        {
            Pending.RetryAt = Now + 100ULL;
            return false;
        }

        RecoverPendingState(Pending);
        PendingCheatBotVisualCommits.erase(Existing);
        AIDebugLogger::MissingFeature("BoundedCheatBotCosmeticFallback",
            "the deferred native refresh and exact-part fallback did not produce a complete pawn mesh");
        return false;
    }

    FPlayerAIPendingCheatBotVisualCommit Transaction{};
    Transaction.World = TWeakObjectPtr<UWorld>(UWorld::GetWorld());
    Transaction.Controller = TWeakObjectPtr<AFortPlayerControllerAthena>(Controller);
    Transaction.PlayerState = TWeakObjectPtr<AFortPlayerStateAthena>(PlayerState);
    Transaction.Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
    Transaction.Character = TWeakObjectPtr<UObject>(Character);
    for (int Index = 0;
         Index < PlayerAICharacterPartSlotCount; Index++)
    {
        Transaction.Parts[Index] = TWeakObjectPtr<UObject>(const_cast<UObject*>(Parts[Index]));
        if (Parts[Index])
        {
            Transaction.NonNullPartMask |= static_cast<uint8>(1u << Index);
        }
    }
    Transaction.PreviousLoadouts[0] = PlayerAICaptureCharacterLoadout(
            Pawn, "CosmeticLoadout", L"CosmeticLoadout");
    Transaction.PreviousLoadouts[1] = PlayerAICaptureCharacterLoadout(
            Pawn, "BaseCosmeticLoadout", L"BaseCosmeticLoadout");
    Transaction.PreviousLoadouts[2] = PlayerAICaptureCharacterLoadout(
            Pawn, "AppliedCosmeticLoadout", L"AppliedCosmeticLoadout");
    Transaction.PreviousLoadouts[3] = PlayerAICaptureCharacterLoadout(
            Controller, "CosmeticLoadoutPC", L"CosmeticLoadoutPC");
    Transaction.PreviousLoadouts[4] = PlayerAICaptureCharacterLoadout(
            Controller, "CustomizationLoadout", L"CustomizationLoadout");
    Transaction.PreviousLoadouts[5] = PlayerAICaptureCharacterLoadout(
            Controller, "CosmeticLoadoutBC", L"CosmeticLoadoutBC");
    Transaction.PreviousHeadAccessoryPreference = PlayerAICaptureReflectedBool(
            PlayerState, "bShowHeroHeadAccessories", L"bShowHeroHeadAccessories");
    Transaction.PreviousBackpackPreference = PlayerAICaptureReflectedBool(
            PlayerState, "bShowHeroBackpack", L"bShowHeroBackpack");
    if (PreviousMetadata)
        Transaction.PreviousMetadata = *PreviousMetadata;
    Transaction.PreviousParts = PlayerAICaptureCharacterParts(PlayerState, Pawn);
    Transaction.QueuedAt = Now;

    if (!Transaction.PreviousParts.bValid)
    {
        AIDebugLogger::MissingFeature("CheatBotCosmeticRollbackSnapshot",
            "the bot keeps its engine appearance because its prior character-part state could not be captured safely");
        return false;
    }

    PlayerAITryRepairPawnPlayerStateBinding(PlayerState, Pawn);

    if (!PublishAuthoritativeInputs(true))
    {
        RecoverPendingState(Transaction);
        return false;
    }

    bool bApplyInvoked = false;
    const bool bApplyAccepted = PlayerAITryApplyCharacterCosmetics(
            PlayerState, Pawn, Parts, &bApplyInvoked);
    if (bApplyInvoked)
    {
        bPlayerAILastCheatBotVisualCallPerformedNativeWork = true;
        Transaction.Phase = EPlayerAICheatBotVisualCommitPhase::AwaitNativeRefresh;
        Transaction.NativeRefreshStartedAt = Now;
        Transaction.bApplyCosmeticsRejected = !bApplyAccepted;
        Transaction.RetryAt = Now + NativeRefreshWaitMs;
        PlayerAINoteBotSkinProgress();
        PendingCheatBotVisualCommits[Pawn] = std::move(Transaction);
        return false;
    }

    if (PlayerAITryServerSetCosmeticLoadout(Controller, Character, true))
    {
        bPlayerAILastCheatBotVisualCallPerformedNativeWork = true;
        Transaction.Phase = EPlayerAICheatBotVisualCommitPhase::AwaitNativeRefresh;
        Transaction.NativeRefreshStartedAt = Now;
        Transaction.bNativeLoadoutSetterAttempted = true;
        Transaction.RetryAt = Now + NativeRefreshWaitMs;
        PlayerAINoteBotSkinProgress();
        PendingCheatBotVisualCommits[Pawn] = std::move(Transaction);
        return false;
    }

    Transaction.Phase = EPlayerAICheatBotVisualCommitPhase::DispatchExactParts;
    Transaction.RetryAt = Now + 1ULL;
    PlayerAINoteBotSkinProgress();
    PendingCheatBotVisualCommits[Pawn] = std::move(Transaction);
    return false;
}

static bool PlayerAITryGetResidentPlaceholderParts(AFortPlayerStateAthena* TargetPlayerState,
    const UObject* OutParts[PlayerAICharacterPartSlotCount], const UObject*& OutHero)
{
    OutHero = nullptr;
    if (!TargetPlayerState || !OutParts)
        return false;

    auto PartClass = UCustomCharacterPart::StaticClass();
    auto HeroClass = UFortHeroType::StaticClass();
    if (!PartClass)
        return false;

    auto ClearCache = []()
        {
            for (int Index = 0;
                 Index < PlayerAICharacterPartSlotCount; Index++)
            {
                PlayerAIResidentPlaceholderParts[Index] = nullptr;
                PlayerAIResidentPlaceholderPartIndices[Index] = -1;
            }
            PlayerAIResidentPlaceholderHero = nullptr;
            PlayerAIResidentPlaceholderHeroIndex = -1;
        };

    auto CopyCached = [&]() -> bool
        {
            if (!PlayerAIResidentPlaceholderParts[0] || !PlayerAIResidentPlaceholderParts[1])
            {
                return false;
            }

            for (int Index = 0;
                 Index < PlayerAICharacterPartSlotCount; Index++)
            {
                auto Part = PlayerAIResidentPlaceholderParts[Index];
                if (!Part)
                    continue;

                if (!PlayerAIIsLiveSupportObject(Part) || !Part->IsA(PartClass) || Part->Index !=
                        PlayerAIResidentPlaceholderPartIndices[Index])
                {
                    ClearCache();
                    return false;
                }

                auto CharacterPart = static_cast<const UCustomCharacterPart*>(Part);
                if (!CharacterPart->HasCharacterPartType() ||
                    CharacterPart->CharacterPartType != Index)
                {
                    ClearCache();
                    return false;
                }
            }

            if (PlayerAIResidentPlaceholderHero && (!HeroClass || !PlayerAIIsLiveSupportObject(
                    PlayerAIResidentPlaceholderHero) ||
                 !PlayerAIResidentPlaceholderHero->IsA(HeroClass) ||
                 PlayerAIResidentPlaceholderHero->Index != PlayerAIResidentPlaceholderHeroIndex))
            {
                PlayerAIResidentPlaceholderHero = nullptr;
                PlayerAIResidentPlaceholderHeroIndex = -1;
            }

            memcpy(OutParts, PlayerAIResidentPlaceholderParts,
                sizeof(PlayerAIResidentPlaceholderParts));
            OutHero = PlayerAIResidentPlaceholderHero;
            return true;
        };

    if (CopyCached())
        return true;

    const ULONGLONG Now = GetTickCount64();
    if (PlayerAIResidentPlaceholderRetryAt > Now)
        return false;

    auto GameState = VersionFeatureAdapter::GetGameState();
    if (!GameState)
        return false;

    auto TryPartArray = [&](UObject** Candidate, int32 CandidateSlotCount) -> bool
        {
            if (!Candidate || CandidateSlotCount <PlayerAILegacyCharacterPartSlotCount)
            {
                return false;
            }

            const int32 SlotCount = (std::min)(CandidateSlotCount, PlayerAICharacterPartSlotCount);
            if (!SDK::MemReadable(Candidate, sizeof(UObject*) * SlotCount))
            {
                return false;
            }

            const UObject* Validated[PlayerAICharacterPartSlotCount]{};
            for (int Index = 0; Index < SlotCount; Index++)
            {
                auto Part = Candidate[Index];
                if (!Part)
                    continue;

                if (!PlayerAIIsLiveSupportObject(Part) || !Part->IsA(PartClass))
                {
                    return false;
                }

                auto CharacterPart = static_cast<const UCustomCharacterPart*>(Part);
                if (!CharacterPart->HasCharacterPartType() ||
                    CharacterPart->CharacterPartType != Index)
                {
                    return false;
                }
                Validated[Index] = Part;
            }

            if (!Validated[0] || !Validated[1])
                return false;

            memcpy(PlayerAIResidentPlaceholderParts, Validated, sizeof(Validated));
            for (int Index = 0;
                 Index < PlayerAICharacterPartSlotCount; Index++)
            {
                PlayerAIResidentPlaceholderPartIndices[Index] =
                    Validated[Index] ? Validated[Index]->Index : -1;
            }
            return true;
        };

    const int32 DonorCount = GameState->PlayerArray.Num();
    if (DonorCount <= 0)
        return false;

    static constexpr int32 PlayerAIPlaceholderDonorsPerAttempt = 8;
    const int32 InspectCount = (std::min)(DonorCount, PlayerAIPlaceholderDonorsPerAttempt);
    const size_t Start = PlayerAIResidentPlaceholderDonorCursor % static_cast<size_t>(DonorCount);
    PlayerAIResidentPlaceholderRetryAt = Now + 250ULL;

    for (int32 Step = 0; Step < InspectCount; Step++)
    {
        const size_t DonorIndex = (Start + static_cast<size_t>(Step)) %
            static_cast<size_t>(DonorCount);
        PlayerAIResidentPlaceholderDonorCursor = (DonorIndex + 1) % static_cast<size_t>(DonorCount);
        auto DonorPlayerState = GameState->PlayerArray[static_cast<int32>(DonorIndex)];
        if (!DonorPlayerState || DonorPlayerState == TargetPlayerState ||
            !PlayerAIIsLiveSupportObject(DonorPlayerState) || (DonorPlayerState->HasbIsABot() &&
             DonorPlayerState->bIsABot))
        {
            continue;
        }

        UObject** Candidate = nullptr;
        int32 CandidateSlotCount = 0;
        bool bFoundParts = PlayerAIResolveStructCharacterPartArray(
                DonorPlayerState, "CharacterData", "CustomCharacterData",
                L"/Script/FortniteGame.CustomCharacterData", Candidate, nullptr, nullptr, nullptr,
                &CandidateSlotCount) && TryPartArray(Candidate, CandidateSlotCount);
        if (!bFoundParts)
        {
            Candidate = nullptr;
            CandidateSlotCount = 0;
            bFoundParts = PlayerAIResolveFixedCharacterPartArray(
                    DonorPlayerState, "CharacterParts", Candidate, &CandidateSlotCount) &&
                TryPartArray(Candidate, CandidateSlotCount);
        }
        if (!bFoundParts)
        {
            Candidate = nullptr;
            CandidateSlotCount = 0;
            bFoundParts = PlayerAIResolveStructCharacterPartArray(
                    DonorPlayerState, "CharacterParts", "CustomCharacterParts",
                    L"/Script/FortniteGame.CustomCharacterParts",
                    Candidate, nullptr, nullptr, nullptr, &CandidateSlotCount) &&
                TryPartArray(Candidate, CandidateSlotCount);
        }
        if (!bFoundParts)
        {
            Candidate = nullptr;
            CandidateSlotCount = 0;
            bFoundParts = PlayerAIResolveFixedCharacterPartArray(
                    DonorPlayerState, "LocalCharacterParts", Candidate, &CandidateSlotCount) &&
                TryPartArray(Candidate, CandidateSlotCount);
        }
        if (!bFoundParts)
            continue;

        auto Hero = DonorPlayerState->HasHeroType() ? DonorPlayerState->HeroType : nullptr;
        if (HeroClass && PlayerAIIsLiveSupportObject(Hero) && Hero->IsA(HeroClass))
        {
            PlayerAIResidentPlaceholderHero = Hero;
            PlayerAIResidentPlaceholderHeroIndex = Hero->Index;
        }

        PlayerAIResidentPlaceholderRetryAt = 0;
        return CopyCached();
    }

    ClearCache();
    return false;
}

static bool PlayerAIApplyDefaultCosmeticsInternal(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, EPlayerAICosmeticLoadPolicy LoadPolicy,
    bool bAllowResidentDonorFallback, bool bReportUnavailable)
{
    if (!PlayerState)
        return false;

    const bool bAllowSynchronousLoad = LoadPolicy ==
        EPlayerAICosmeticLoadPolicy::AllowSynchronousLoad;

    if (bDefaultCommandoResolved && PlayerAIDefaultCommando &&
        !PlayerAIIsLiveSupportObject(PlayerAIDefaultCommando))
    {
        bDefaultCommandoResolved = false;
        PlayerAIDefaultCommando = nullptr;
        bPlayerAIRequestedDefaultSynchronousAttempted = false;
    }

    if (!bDefaultCommandoResolved || (bAllowSynchronousLoad && !PlayerAIDefaultCommando))
    {
        bDefaultCommandoResolved = true;
        PlayerAIDefaultCommando = PlayerAIResolveDefaultCosmetic(
                L"/Game/Athena/Heroes/HID_001_Athena_Commando_F.HID_001_Athena_Commando_F",
                UFortHeroType::StaticClass(), bAllowSynchronousLoad);

        // This fallback is absent on 2.50, so only resolve it once the original HID is unavailable.
        if (!PlayerAIDefaultCommando)
        {
            PlayerAIDefaultCommando = PlayerAIResolveDefaultCosmetic(
                L"/Game/Athena/Heroes/HID_Commando_Athena_01.HID_Commando_Athena_01",
                UFortHeroType::StaticClass(), bAllowSynchronousLoad);
        }
    }

    if (bDefaultPartsResolved && ((PlayerAIDefaultHead &&
          !PlayerAIIsLiveSupportObject(PlayerAIDefaultHead)) || (PlayerAIDefaultBody &&
          !PlayerAIIsLiveSupportObject(PlayerAIDefaultBody)) || (PlayerAIDefaultBackpack &&
          !PlayerAIIsLiveSupportObject(PlayerAIDefaultBackpack))))
    {
        bDefaultPartsResolved = false;
        PlayerAIDefaultHead = nullptr;
        PlayerAIDefaultBody = nullptr;
        PlayerAIDefaultBackpack = nullptr;
        bPlayerAIRequestedDefaultSynchronousAttempted = false;
    }

    if (!bDefaultPartsResolved || (bAllowSynchronousLoad &&
         (!PlayerAIDefaultHead || !PlayerAIDefaultBody)))
    {
        bDefaultPartsResolved = true;
        PlayerAIDefaultHead = PlayerAIResolveDefaultCosmetic(
            L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1",
            UCustomCharacterPart::StaticClass(), bAllowSynchronousLoad);
        PlayerAIDefaultBody = PlayerAIResolveDefaultCosmetic(
            L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01",
            UCustomCharacterPart::StaticClass(), bAllowSynchronousLoad);
        PlayerAIDefaultBackpack = PlayerAIResolveDefaultCosmetic(
            L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack",
            UCustomCharacterPart::StaticClass(), bAllowSynchronousLoad);
    }

    auto HeroClass = UFortHeroType::StaticClass();
    auto PartClass = UCustomCharacterPart::StaticClass();

    if (!HeroClass || !PartClass || !PlayerAIIsLiveSupportObject(PlayerAIDefaultCommando) ||
        !PlayerAIDefaultCommando->IsA(HeroClass) || !PlayerAIIsLiveSupportObject(
            PlayerAIDefaultHead) || !PlayerAIDefaultHead->IsA(PartClass) ||
        !PlayerAIIsLiveSupportObject(PlayerAIDefaultBody) || !PlayerAIDefaultBody->IsA(PartClass))
    {
        if (bAllowResidentDonorFallback)
        {
            const UObject* PlaceholderParts[PlayerAICharacterPartSlotCount]{};
            const UObject* PlaceholderHero = nullptr;
            if (Pawn && PlayerAITryGetResidentPlaceholderParts(
                    PlayerState, PlaceholderParts, PlaceholderHero))
            {
                if (PlaceholderHero)
                    PlayerAIWriteHeroType(PlayerState, PlaceholderHero);

                EFortCustomGender PlaceholderGender = EFortCustomGender::Invalid;
                if (PlayerAIResolveConcreteGender(PlayerState, nullptr,
                        PlaceholderParts, PlaceholderGender))
                {
                    PlayerAIWriteGender(PlayerState, PlaceholderGender);
                }
                uint8 PlaceholderBodyType = 0;
                if (PlayerAIResolveConcreteBodyType(PlayerState, PlaceholderParts,
                        PlaceholderBodyType))
                {
                    PlayerAIWriteBodyType(PlayerState, PlaceholderBodyType);
                }

                if (PlayerAIFinishCosmetics(PlayerState, Pawn, PlaceholderParts))
                {
                    return true;
                }
            }
        }

        if (bReportUnavailable)
        {
            AIDebugLogger::MissingFeature("DefaultCharacterParts",
                "complete default cosmetics unavailable; PlayerAI keeps its engine appearance");
        }
        return false;
    }

    const UObject* Parts[PlayerAICharacterPartSlotCount] =
    {
        PlayerAIDefaultHead, PlayerAIDefaultBody, nullptr, PlayerAIDefaultBackpack, nullptr,
        nullptr,
    };

    PlayerAIWriteHeroType(PlayerState, PlayerAIDefaultCommando);
    PlayerAIWriteGender(PlayerState, EFortCustomGender::Female);
    uint8 DefaultBodyType = 0;
    if (PlayerAIResolveConcreteBodyType(PlayerState, Parts, DefaultBodyType))
    {
        PlayerAIWriteBodyType(PlayerState, DefaultBodyType);
    }

    return PlayerAIFinishCosmetics(PlayerState, Pawn, Parts);
}

bool VersionFeatureAdapter::ApplyDefaultCosmetics(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, EPlayerAICosmeticLoadPolicy LoadPolicy)
{
    return PlayerAIApplyDefaultCosmeticsInternal(PlayerState, Pawn, LoadPolicy, true, true);
}

bool VersionFeatureAdapter::ApplyFixedDefaultCosmetics(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, EPlayerAICosmeticLoadPolicy LoadPolicy)
{
    return PlayerAIApplyDefaultCosmeticsInternal(PlayerState, Pawn, LoadPolicy, false, true);
}

static const wchar_t* PlayerAIKnownSkinNames[] =
{
    L"CID_001_Athena_Commando_F_Default", L"CID_002_Athena_Commando_F_Default",
    L"CID_003_Athena_Commando_F_Default", L"CID_004_Athena_Commando_F_Default",
    L"CID_005_Athena_Commando_M_Default", L"CID_006_Athena_Commando_M_Default",
    L"CID_007_Athena_Commando_M_Default", L"CID_008_Athena_Commando_M_Default",
    L"CID_009_Athena_Commando_M", L"CID_010_Athena_Commando_M",
    L"CID_011_Athena_Commando_M", L"CID_012_Athena_Commando_M",
    L"CID_013_Athena_Commando_F", L"CID_014_Athena_Commando_F",
    L"CID_015_Athena_Commando_F", L"CID_016_Athena_Commando_F",
    L"CID_017_Athena_Commando_M", L"CID_018_Athena_Commando_M",
    L"CID_019_Athena_Commando_M", L"CID_020_Athena_Commando_M",
    L"CID_021_Athena_Commando_F", L"CID_022_Athena_Commando_F",
    L"CID_023_Athena_Commando_F", L"CID_024_Athena_Commando_F",
    L"CID_025_Athena_Commando_M", L"CID_026_Athena_Commando_M",
    L"CID_028_Athena_Commando_F", L"CID_029_Athena_Commando_F_Halloween",
    L"CID_030_Athena_Commando_M_Halloween", L"CID_031_Athena_Commando_M_Retro",
    L"CID_032_Athena_Commando_M_Medieval", L"CID_033_Athena_Commando_F_Medieval",
    L"CID_035_Athena_Commando_M_Medieval", L"CID_039_Athena_Commando_F_Disco",
    L"CID_045_Athena_Commando_M_HolidaySweater", L"CID_048_Athena_Commando_F_HolidaySweater",
    L"CID_051_Athena_Commando_M_HolidayElf", L"CID_052_Athena_Commando_F_PSBlue",
};

struct FPlayerAIRawScriptArray
{
    uint8* Data = nullptr;
    int32 Num = 0;
    int32 Max = 0;
};

static bool PlayerAIParameterRangeFits(size_t ParamsSize, uint32 Offset, size_t ValueSize)
{
    return Offset != (uint32)-1 && ValueSize <= ParamsSize && static_cast<size_t>(Offset) <=
            ParamsSize - ValueSize;
}

static bool PlayerAITryProcessCosmeticFunction(
    const UObject* Object, UFunction* Function, void* Params)
{
    if (!Object || !Function || !Params)
        return false;

    GGuardedNativeCallDepth++;
    bool bOk = false;

    __try
    {
        Object->ProcessEvent(Function, Params);
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GGuardedNativeCallDepth--;
    return bOk;
}

static size_t PlayerAICosmeticFunctionParamSize(const UFunction* Function)
{
    if (!Function)
        return 0;

    size_t Size = Function->GetPropertiesSize();

    if (Size < 0x20)
        Size = 0x100;

    return Size <= 0x10000 ? Size : 0;
}

static std::string PlayerAILowerASCII(std::string Value)
{
    for (auto& Character : Value)
    {
        if (Character >= 'A' && Character <= 'Z')
            Character = static_cast<char>(Character - 'A' + 'a');
    }

    return Value;
}

static bool PlayerAIIsPlayerSkinName(const std::string& Name)
{
    const auto Lower = PlayerAILowerASCII(Name);

    return Lower.rfind("cid_", 0) == 0 && Lower.find("cid_npc") == std::string::npos &&
        Lower.find("cid_vip") == std::string::npos && Lower.find("cid_tbd") == std::string::npos;
}

static void PlayerAIWriteFNameStringUnsafe(const FName* Name, char* Output, size_t OutputSize)
{
    auto Value = Name->ToString();
    strncpy_s(Output, OutputSize, Value.c_str(), _TRUNCATE);
}

static bool PlayerAITryWriteFNameString(const FName* Name, char* Output, size_t OutputSize)
{
    if (!Name || !Output || OutputSize == 0)
        return false;

    Output[0] = '\0';
    GGuardedNativeCallDepth++;
    bool bOk = false;

    __try
    {
        PlayerAIWriteFNameStringUnsafe(Name, Output, OutputSize);
        bOk = Output[0] != '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GGuardedNativeCallDepth--;
    return bOk;
}

// Fortnite 20 compacted FName from eight bytes to four, so treat the returned array as raw bytes.
static bool PlayerAIQueryCurrentBuildSkinCatalog()
{
    if (bPrimaryAssetCosmeticFunctionsDisabled)
        return false;

    auto SystemLibrary = UKismetSystemLibrary::GetDefaultObj();
    auto Function = SystemLibrary ? SystemLibrary->GetFunction("GetPrimaryAssetIdList") : nullptr;

    if (!SystemLibrary)
        return false;
    if (!Function)
    {
        bPrimaryAssetCosmeticFunctionsDisabled = true;
        AIDebugLogger::MissingFeature("CharacterPrimaryAssetCatalog",
            "this build has no primary-asset catalog API; resident skins remain available");
        return false;
    }

    const uint32 TypeOffset = Function->GetOffset("PrimaryAssetType");
    const uint32 OutputOffset = Function->GetOffset("OutPrimaryAssetIdList");
    const size_t ParamsSize = PlayerAICosmeticFunctionParamSize(Function);
    const int32 NameSize = VersionInfo.FortniteVersion >= 20.00 ? 0x4 : 0x8;
    const int32 IdSize = NameSize * 2;

    {
        auto PrimaryAssetIdStruct = FindStruct("PrimaryAssetId");
        const auto ReflectedIdSize = PrimaryAssetIdStruct
            ? PrimaryAssetIdStruct->GetPropertiesSize() : 0;
        const auto ReflectedNameOffset = PrimaryAssetIdStruct ? PrimaryAssetIdStruct->GetOffset(
                "PrimaryAssetName") : (uint32)-1;

        if (ReflectedIdSize != IdSize || ReflectedNameOffset != (uint32)NameSize)
        {
            bPrimaryAssetCosmeticFunctionsDisabled = true;
            AIDebugLogger::MissingFeature("CharacterPrimaryAssetCatalogLayout",
                "this build's asset-id layout is unknown; resident skins remain available");
            return false;
        }
    }

    // Stable across the sampled ABI endpoints: 10.40 with 8-byte names and FN20+ with compact 4-byte names.
    const bool bCatalogSchemaValid = TypeOffset == 0 && OutputOffset == 0x8 &&
        PlayerAIParameterRangeFits(ParamsSize, TypeOffset, NameSize) && PlayerAIParameterRangeFits(
            ParamsSize, OutputOffset, sizeof(FPlayerAIRawScriptArray));

    if (!bCatalogSchemaValid || IdSize > (int32)sizeof(
            FPlayerAISkinCatalogEntry::RawPrimaryAssetId))
    {
        bPrimaryAssetCosmeticFunctionsDisabled = true;
        AIDebugLogger::MissingFeature("CharacterPrimaryAssetCatalogParameters",
            "this build's catalog function layout is unknown; resident skins remain available");
        return false;
    }

    auto Params = FMemory::Malloc(ParamsSize);
    if (!Params)
        return false;

    memset(Params, 0, ParamsSize);
    FName CharacterAssetType{};
    CharacterAssetType = FName(L"AthenaCharacter");
    memcpy((uint8*)Params + TypeOffset, &CharacterAssetType, NameSize);

    LARGE_INTEGER CatalogCallFrequency{};
    LARGE_INTEGER CatalogCallStartedAt{};
    LARGE_INTEGER CatalogCallFinishedAt{};
    const bool bCanMeasureCatalogCall = QueryPerformanceFrequency(&CatalogCallFrequency) &&
        CatalogCallFrequency.QuadPart > 0 && QueryPerformanceCounter(&CatalogCallStartedAt);
    const bool bCalled = PlayerAITryProcessCosmeticFunction(SystemLibrary, Function, Params);
    const bool bMeasuredCatalogCall = bCanMeasureCatalogCall &&
        QueryPerformanceCounter(&CatalogCallFinishedAt);
    const double CatalogCallElapsedMs = bMeasuredCatalogCall ? static_cast<double>(
            CatalogCallFinishedAt.QuadPart - CatalogCallStartedAt.QuadPart) * 1000.0 /
            static_cast<double>(CatalogCallFrequency.QuadPart) : 0.0;
    if (CatalogCallElapsedMs >= 4.0)
    {
        AIDebugLogger::Log("Performance", "GetPrimaryAssetIdList(AthenaCharacter) took %.2f ms",
            CatalogCallElapsedMs);
    }
    FPlayerAIRawScriptArray Output{};

    if (!bCalled)
    {
        bPrimaryAssetCosmeticFunctionsDisabled = true;
        FMemory::Free(Params);
        AIDebugLogger::MissingFeature("CharacterPrimaryAssetCatalog",
            "the current build rejected catalog lookup; resident skins remain available");
        return false;
    }

    memcpy(&Output, (uint8*)Params + OutputOffset, sizeof(Output));

    const bool bValidOutput = Output.Num > 0 && Output.Num <= 100000 && Output.Max >= Output.Num &&
        Output.Max <= 100000 && Output.Data && SDK::MemReadable(Output.Data,
            static_cast<size_t>(Output.Num) * IdSize);

    FMemory::Free(Params);

    if (!bValidOutput)
        return false;

    if (PendingSkinCatalogData)
    {
        FMemory::Free(Output.Data);
        return true;
    }

    PendingSkinCatalogData = Output.Data;
    PendingSkinCatalogCount = Output.Num;
    PendingSkinCatalogIdSize = IdSize;
    PendingSkinCatalogNameSize = NameSize;
    PendingSkinCatalogCursor = 0;
    PendingSkinCatalogEntries.clear();
    PendingSkinCatalogSeenNames.clear();
    PendingSkinCatalogByLowerName.clear();
    return true;
}

static void PlayerAIClearPendingSkinCatalog()
{
    if (PendingSkinCatalogData)
        FMemory::Free(PendingSkinCatalogData);

    PendingSkinCatalogData = nullptr;
    PendingSkinCatalogCount = 0;
    PendingSkinCatalogIdSize = 0;
    PendingSkinCatalogNameSize = 0;
    PendingSkinCatalogCursor = 0;
    PendingSkinCatalogEntries.clear();
    PendingSkinCatalogSeenNames.clear();
    PendingSkinCatalogByLowerName.clear();
}

static void PlayerAITickSkinCatalogDecode(int Budget)
{
    if (!PendingSkinCatalogData || Budget <= 0 || PendingSkinCatalogCount <= 0 ||
        PendingSkinCatalogIdSize <= 0 || PendingSkinCatalogNameSize <= 0)
    {
        return;
    }

    const int32 End = (std::min)(PendingSkinCatalogCursor + Budget, PendingSkinCatalogCount);

    for (int32 Index = PendingSkinCatalogCursor;
         Index < End; Index++)
    {
        auto RawId = PendingSkinCatalogData + static_cast<size_t>(Index) * PendingSkinCatalogIdSize;
        FName AssetName{};
        memcpy(&AssetName, RawId + PendingSkinCatalogNameSize, PendingSkinCatalogNameSize);
        char NameBuffer[512]{};

        if (!PlayerAITryWriteFNameString(&AssetName, NameBuffer, sizeof(NameBuffer)))
        {
            bPrimaryAssetCosmeticFunctionsDisabled = true;
            PlayerAIClearPendingSkinCatalog();
            AIDebugLogger::MissingFeature("CharacterPrimaryAssetCatalogDecode",
                "a raw asset name could not be decoded; resident skins remain available");
            return;
        }

        std::string Name(NameBuffer);
        if (!PlayerAIIsPlayerSkinName(Name))
            continue;

        const auto LowerName = PlayerAILowerASCII(Name);
        if (!PendingSkinCatalogSeenNames.insert(LowerName).second)
        {
            continue;
        }

        FPlayerAISkinCatalogEntry Entry{};
        memcpy(Entry.RawPrimaryAssetId, RawId, PendingSkinCatalogIdSize);
        Entry.PrimaryAssetIdSize = PendingSkinCatalogIdSize;
        Entry.Name = Name;
        PendingSkinCatalogByLowerName.emplace(LowerName, PendingSkinCatalogEntries.size());
        PendingSkinCatalogEntries.push_back(std::move(Entry));
    }

    if (End > PendingSkinCatalogCursor)
    {
        PendingSkinCatalogCursor = End;
        PlayerAINoteBotSkinProgress();
    }
    if (PendingSkinCatalogCursor <PendingSkinCatalogCount)
    {
        return;
    }

    auto CompletedCatalog = std::move(PendingSkinCatalogEntries);
    auto CompletedNameIndex = std::move(PendingSkinCatalogByLowerName);
    PlayerAIClearPendingSkinCatalog();

    if (CompletedCatalog.empty())
    {
        SkinCatalogRetryTicks = 300;
        return;
    }

    CachedSkinCatalog = std::move(CompletedCatalog);
    CachedSkinCatalogByLowerName = std::move(CompletedNameIndex);
    bSkinCatalogReady = true;
    AIDebugLogger::Log("Cosmetics",
        "%d current-build character skins discovered through the primary-asset catalog",
        (int)CachedSkinCatalog.size());
}

static void PlayerAITryBuildSkinCatalog(bool bForceNow)
{
    if (bSkinCatalogReady || bPrimaryAssetCosmeticFunctionsDisabled || PendingSkinCatalogData)
    {
        return;
    }

    if (!bForceNow)
    {
        if (SkinCatalogAttempts >= 20)
            return;

        if (SkinCatalogRetryTicks > 0)
        {
            SkinCatalogRetryTicks--;
            return;
        }
    }
    else if (SkinCatalogAttempts > 0 && LastSkinCatalogAttemptServerTime ==
            PlayerAILastServerTickTime)
    {
        return;
    }

    SkinCatalogAttempts++;
    LastSkinCatalogAttemptServerTime = PlayerAILastServerTickTime;
    const bool bScheduled = PlayerAIQueryCurrentBuildSkinCatalog();
    SkinCatalogRetryTicks = bScheduled ? 0 : 300;
}

static UAthenaCharacterItemDefinition* PlayerAITryResolveCatalogEntry(
    const FPlayerAISkinCatalogEntry& Entry, EPlayerAICosmeticLoadPolicy LoadPolicy)
{
    if (Entry.PrimaryAssetIdSize <= 0 || Entry.PrimaryAssetIdSize >
            (int32)sizeof(Entry.RawPrimaryAssetId) || bPrimaryAssetCosmeticFunctionsDisabled)
    {
        return nullptr;
    }

    auto SystemLibrary = UKismetSystemLibrary::GetDefaultObj();
    auto CharacterClass = UAthenaCharacterItemDefinition::StaticClass();

    if (!SystemLibrary || !CharacterClass)
        return nullptr;

    // Fast resident-object lookup first.
    if (auto Function = SystemLibrary->GetFunction("GetObjectFromPrimaryAssetId"))
    {
        const uint32 InputOffset = Function->GetOffset("PrimaryAssetId");
        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        const size_t ParamsSize = PlayerAICosmeticFunctionParamSize(Function);

        const size_t IdSize = static_cast<size_t>(Entry.PrimaryAssetIdSize);
        const bool bObjectGetterSchemaValid = InputOffset == 0 && ReturnOffset == IdSize &&
            PlayerAIParameterRangeFits(ParamsSize, InputOffset, IdSize) &&
            PlayerAIParameterRangeFits(ParamsSize, ReturnOffset, sizeof(UObject*));

        if (bObjectGetterSchemaValid)
        {
            auto Params = FMemory::Malloc(ParamsSize);
            if (Params)
            {
                memset(Params, 0, ParamsSize);
                memcpy((uint8*)Params + InputOffset, Entry.RawPrimaryAssetId,
                    Entry.PrimaryAssetIdSize);

                const bool bCalled = PlayerAITryProcessCosmeticFunction(
                        SystemLibrary, Function, Params);
                UObject* Result = nullptr;
                memcpy(&Result, (uint8*)Params + ReturnOffset, sizeof(Result));
                FMemory::Free(Params);

                if (!bCalled)
                    bPrimaryAssetCosmeticFunctionsDisabled = true;

                if (PlayerAIIsLiveSupportObject(Result) && Result->IsA(CharacterClass))
                {
                    return (UAthenaCharacterItemDefinition*)
                        Result;
                }
            }
        }
    }

    if (LoadPolicy != EPlayerAICosmeticLoadPolicy::AllowSynchronousLoad ||
        bPrimaryAssetCosmeticFunctionsDisabled)
    {
        return nullptr;
    }

    if (auto Function = SystemLibrary->GetFunction("GetSoftObjectReferenceFromPrimaryAssetId"))
    {
        const uint32 InputOffset = Function->GetOffset("PrimaryAssetId");
        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        const size_t ParamsSize = PlayerAICosmeticFunctionParamSize(Function);
        const size_t SoftObjectSize = FSoftObjectPtr::Size();

        const size_t IdSize = static_cast<size_t>(Entry.PrimaryAssetIdSize);
        const bool bSoftGetterSchemaValid = InputOffset == 0 && ReturnOffset == IdSize &&
            PlayerAIParameterRangeFits(ParamsSize, InputOffset, IdSize) &&
            PlayerAIParameterRangeFits(ParamsSize, ReturnOffset, SoftObjectSize);

        if (bSoftGetterSchemaValid)
        {
            auto Params = FMemory::Malloc(ParamsSize);
            if (Params)
            {
                memset(Params, 0, ParamsSize);
                memcpy((uint8*)Params + InputOffset, Entry.RawPrimaryAssetId,
                    Entry.PrimaryAssetIdSize);

                const bool bCalled = PlayerAITryProcessCosmeticFunction(
                        SystemLibrary, Function, Params);
                const UObject* Result = nullptr;

                if (bCalled)
                {
                    auto SoftObject = (FSoftObjectPtr*)((uint8*)Params + ReturnOffset);
                    Result = PlayerAITryResolveReturnedSoftObjectForCommand(
                            SoftObject, CharacterClass);
                }
                else
                {
                    bPrimaryAssetCosmeticFunctionsDisabled = true;
                }

                FMemory::Free(Params);

                if (PlayerAIIsLiveSupportObject(Result) && Result->IsA(CharacterClass))
                {
                    return (UAthenaCharacterItemDefinition*)
                        Result;
                }
            }
        }
    }

    return nullptr;
}

static void PlayerAITickKnownSkinDiscovery(int Budget)
{
    if (bKnownSkinsLoaded || Budget <= 0)
        return;

    auto AddSkin = [&](UAthenaCharacterItemDefinition* CID)
        {
            if (!CID || !CID->HasHeroDefinition() || !CID->HeroDefinition)
                return;

            PlayerAICacheResolvedSkin(CID);
        };

    const int Count = (int)(sizeof(PlayerAIKnownSkinNames) / sizeof(PlayerAIKnownSkinNames[0]));
    const int End = (std::min)(KnownSkinScanCursor + Budget, Count);

    for (int Index = KnownSkinScanCursor;
         Index < End; Index++)
    {
        if (bCosmeticPathLoadDisabled)
            break;

        auto Name = PlayerAIKnownSkinNames[Index];
        UEAllocatedWString Path = UEAllocatedWString(L"/Game/Athena/Items/Cosmetics/Characters/") + Name + L"." + Name;
        auto CID = (UAthenaCharacterItemDefinition*)
            PlayerAITryFindLoadedCosmetic(Path.c_str(),
                UAthenaCharacterItemDefinition::StaticClass());
        AddSkin(CID);
    }

    KnownSkinScanCursor = End;

    if (KnownSkinScanCursor >= Count || bCosmeticPathLoadDisabled)
    {
        bKnownSkinsLoaded = true;
        AIDebugLogger::Log("Cosmetics", "%d resident known skins discovered for PlayerAI",
            (int)CachedSkins.size());
    }
}

static void PlayerAITickLoadedSkinScan(int Budget)
{
    if (bLoadedSkinScanCompleted || Budget <= 0)
        return;

    if (LoadedSkinScanLimit <= 0)
    {
        LoadedSkinScanCursor = 0;
        LoadedSkinScanLimit = TUObjectArray::Num();
        SkinCacheAttempts++;

        if (LoadedSkinScanLimit <= 0)
        {
            bLoadedSkinScanCompleted = true;
            return;
        }
    }

    const int End = (std::min)(LoadedSkinScanCursor + Budget, LoadedSkinScanLimit);

    for (int i = LoadedSkinScanCursor;
         i < End && i < TUObjectArray::Num(); i++)
    {
        auto Object = TUObjectArray::GetObjectByIndex(i);

        if (!PlayerAIIsLiveSupportObject(Object) || Object->IsDefaultObject() ||
            !Object->IsA<UAthenaCharacterItemDefinition>())
            continue;

        auto CID = (UAthenaCharacterItemDefinition*)Object;

        if (!CID->HasHeroDefinition() || !CID->HeroDefinition)
            continue;

        auto RawName = CID->Name.ToString();
        std::string Name(RawName.c_str());

        const bool bValid = Name.find("CID_") != std::string::npos &&
            Name.find("CID_NPC") == std::string::npos &&
            Name.find("CID_VIP") == std::string::npos && Name.find("CID_TBD") == std::string::npos;

        if (!bValid)
            continue;

        PlayerAICacheResolvedSkin(CID);
    }

    if (End > LoadedSkinScanCursor)
    {
        LoadedSkinScanCursor = End;
        PlayerAINoteBotSkinProgress();
    }

    if (LoadedSkinScanCursor >= LoadedSkinScanLimit)
    {
        bLoadedSkinScanCompleted = true;
        LoadedSkinScanCursor = 0;
        LoadedSkinScanLimit = 0;
        AIDebugLogger::Log("Cosmetics", "%d resident player skins available for PlayerAI (scan %d)",
            (int)CachedSkins.size(), SkinCacheAttempts);
    }
}

void VersionFeatureAdapter::TickCosmeticCache()
{
    const bool bHasPendingCheatBotCosmetics = !PendingRandomBotSkins.empty() ||
        !PendingRequestedBotSkinCommits.empty();
    if (VersionInfo.FortniteVersion >= 2.00 && !VersionFeatureAdapter::HasManagedAIControllers() &&
        !bHasPendingCheatBotCosmetics)
    {
        return;
    }

    if (VersionInfo.FortniteVersion >= 2.00)
        PlayerAITryBuildSkinCatalog(false);
    PlayerAITickSkinCatalogDecode(32);

    const bool bNeedsResidentFallback = bPrimaryAssetCosmeticFunctionsDisabled || std::any_of(
            PendingRandomBotSkins.begin(), PendingRandomBotSkins.end(),
            [](const FPlayerAIPendingRandomSkin& Pending)
            {
                return Pending.bUseResidentFallback;
            });
    const bool bCatalogCanServeRequests = !bNeedsResidentFallback && (PendingSkinCatalogData ||
         (bSkinCatalogReady && !CachedSkinCatalog.empty()));
    if (!bCatalogCanServeRequests)
    {
        PlayerAITickKnownSkinDiscovery(4);
        PlayerAITickLoadedSkinScan(64);
    }
    if (!PlayerAITickPendingRequestedBotSkinCommits())
        PlayerAITickPendingRandomBotSkins();
}

bool VersionFeatureAdapter::IsSkinCommitPending(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn)
        return false;
    if (PendingBotSkinCommitPawnCounts.contains(Pawn))
        return true;

    auto Existing = PendingBotSkinSettles.find(Pawn);
    if (Existing == PendingBotSkinSettles.end())
        return false;

    const ULONGLONG Now = GetTickCount64();
    const bool bStillSettling = Existing->second.Pawn.Get() == Pawn &&
        Existing->second.World.Get() == UWorld::GetWorld() && PlayerAIIsLiveSupportActor(Pawn) &&
        Now < Existing->second.Until;
    if (!bStillSettling)
        PendingBotSkinSettles.erase(Existing);
    return bStillSettling;
}

uint64 VersionFeatureAdapter::GetSkinCommitProgressGeneration()
{
    return PlayerAIBotSkinProgressGeneration;
}

bool VersionFeatureAdapter::IsSkinCommitActivelyWorking(AFortPlayerPawnAthena* Pawn)
{
    return Pawn && PendingBotSkinActivePawns.contains(Pawn);
}

void VersionFeatureAdapter::CancelSkinCommit(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn)
        return;

    PlayerAICancelPendingCheatBotVisualCommit(Pawn);

    PendingRequestedBotSkinCommits.erase(std::remove_if(PendingRequestedBotSkinCommits.begin(),
            PendingRequestedBotSkinCommits.end(),
            [Pawn](const FPlayerAIPendingRequestedSkinCommit& Pending)
            {
                return Pending.PawnIdentity == Pawn;
            }), PendingRequestedBotSkinCommits.end());
    PendingRandomBotSkins.erase(std::remove_if(PendingRandomBotSkins.begin(),
            PendingRandomBotSkins.end(), [Pawn](const FPlayerAIPendingRandomSkin& Pending)
            {
                return Pending.PawnIdentity == Pawn;
            }), PendingRandomBotSkins.end());
    PendingBotSkinCommitPawnCounts.erase(Pawn);
    PendingBotSkinActivePawns.erase(Pawn);
    if (PendingRandomBotSkinCursor >= PendingRandomBotSkins.size())
        PendingRandomBotSkinCursor = 0;
}

template <typename SoftObjectType> static bool PlayerAIHasSafeSoftObjectArray(
    const TArray<TSoftObjectPtr<SoftObjectType>>& Array, int MaxCount)
{
    const int Count = Array.Num();
    const uint32 Stride = FSoftObjectPtr::Size();

    return Count > 0 && Count <= MaxCount && Array.Max() >= Count &&
        Stride >= 0x10 && Stride <= 0x80 && Array.Data && SDK::MemReadable(
            Array.Data, (size_t)Count * Stride);
}

static UAthenaCharacterItemDefinition* PlayerAIValidateCharacterDefinition(const UObject* Object)
{
    auto CharacterClass = UAthenaCharacterItemDefinition::StaticClass();

    if (!CharacterClass || !PlayerAIIsLiveSupportObject(Object) || Object->IsDefaultObject() ||
        !Object->IsA(CharacterClass))
    {
        return nullptr;
    }

    auto Character = (UAthenaCharacterItemDefinition*)Object;
    return Character->HasHeroDefinition() && Character->HeroDefinition ? Character : nullptr;
}

static void PlayerAICacheResolvedSkin(UAthenaCharacterItemDefinition* Character)
{
    if (!Character)
        return;

    if (CachedSkinObjects.insert(Character).second)
        CachedSkins.push_back(Character);

    auto RawName = Character->Name.ToString();
    std::string Name(RawName.c_str());
    if (!Name.empty())
    {
        CachedSkinsByLowerName[PlayerAILowerASCII(Name)] = Character;
    }
}

static bool PlayerAITryGetCachedResolvedSkinParts(UAthenaCharacterItemDefinition* Character,
    const UClass* PartClass, const UObject* OutParts[PlayerAICharacterPartSlotCount])
{
    if (!Character || !PartClass || !OutParts)
        return false;

    auto It = CachedResolvedSkinParts.find(Character);
    if (It == CachedResolvedSkinParts.end())
        return false;

    if (!It->second.bAllAuthoredPartsResolved || !PlayerAIIsLiveSupportObject(Character) ||
        Character->Index != It->second.CharacterObjectIndex)
    {
        CachedResolvedSkinParts.erase(It);
        return false;
    }

    for (int Index = 0;
         Index < PlayerAICharacterPartSlotCount; Index++)
    {
        const UObject* Part = It->second.Parts[Index];
        if (Part && (!PlayerAIIsLiveSupportObject(Part) || !Part->IsA(PartClass) || Part->Index !=
                It->second.PartObjectIndices[Index]))
        {
            CachedResolvedSkinParts.erase(It);
            return false;
        }
    }

    if (!It->second.Parts[0] || !It->second.Parts[1])
    {
        CachedResolvedSkinParts.erase(It);
        return false;
    }

    memcpy(OutParts, It->second.Parts, sizeof(It->second.Parts));
    return true;
}

static void PlayerAICacheResolvedSkinParts(UAthenaCharacterItemDefinition* Character,
    const UObject* Parts[PlayerAICharacterPartSlotCount], bool bAllAuthoredPartsResolved)
{
    if (!Character || !Parts || !Parts[0] || !Parts[1] || !bAllAuthoredPartsResolved)
    {
        if (Character)
            CachedResolvedSkinParts.erase(Character);
        return;
    }

    FPlayerAIResolvedSkinParts& Cached = CachedResolvedSkinParts[Character];
    Cached.CharacterObjectIndex = Character->Index;
    Cached.bAllAuthoredPartsResolved = true;
    memcpy(Cached.Parts, Parts, sizeof(Cached.Parts));

    for (int Index = 0;
         Index < PlayerAICharacterPartSlotCount; Index++)
    {
        Cached.PartObjectIndices[Index] = Parts[Index] ? Parts[Index]->Index : -1;
    }
}

static void PlayerAIMarkResolvedSkinVisuallyProven(UAthenaCharacterItemDefinition* Character)
{
    if (!Character || !CachedResolvedSkinParts.contains(Character) || std::find(
            CachedResolvedSkinPool.begin(), CachedResolvedSkinPool.end(), Character) !=
                CachedResolvedSkinPool.end())
    {
        return;
    }

    CachedResolvedSkinPool.push_back(Character);
}

static std::string PlayerAIExtractCharacterName(const char* IdOrPath)
{
    if (!IdOrPath)
        return {};

    std::string Input(IdOrPath);
    const auto Lower = PlayerAILowerASCII(Input);
    const auto Start = Lower.find("cid_");

    if (Start == std::string::npos)
        return {};

    size_t End = Start;
    while (End < Input.size())
    {
        const char Character = Input[End];
        const bool bAllowed = (Character >= 'a' && Character <= 'z') ||
            (Character >= 'A' && Character <= 'Z') || (Character >= '0' && Character <= '9') ||
            Character == '_' || Character == '-';

        if (!bAllowed)
            break;

        End++;
    }

    const auto Name = Input.substr(Start, End - Start);
    return PlayerAIIsPlayerSkinName(Name) ? Name : std::string();
}

UAthenaCharacterItemDefinition* VersionFeatureAdapter::ResolveCharacterSkin(const char* IdOrPath,
    EPlayerAICosmeticLoadPolicy LoadPolicy)
{
    const auto RequestedName = PlayerAIExtractCharacterName(IdOrPath);

    if (RequestedName.empty())
        return nullptr;

    const auto RequestedLower = PlayerAILowerASCII(RequestedName);

    auto CachedByName = CachedSkinsByLowerName.find(RequestedLower);
    if (CachedByName != CachedSkinsByLowerName.end())
    {
        if (auto Character = PlayerAIValidateCharacterDefinition(CachedByName->second))
        {
            return Character;
        }

        CachedSkinsByLowerName.erase(CachedByName);
    }

    if (LoadPolicy == EPlayerAICosmeticLoadPolicy::AllowSynchronousLoad &&
        !CachedSkinCatalog.empty())
    {
        auto CatalogMatch = CachedSkinCatalogByLowerName.find(RequestedLower);
        if (CatalogMatch != CachedSkinCatalogByLowerName.end() && CatalogMatch->second <
                CachedSkinCatalog.size())
        {
            const auto& Entry = CachedSkinCatalog[CatalogMatch->second];

            if (auto Character = PlayerAIValidateCharacterDefinition(PlayerAITryResolveCatalogEntry(
                            Entry, LoadPolicy)))
            {
                PlayerAICacheResolvedSkin(Character);
                return Character;
            }
        }
    }

    std::string RequestedPath(IdOrPath ? IdOrPath : "");
    const auto RequestedPathLower = PlayerAILowerASCII(RequestedPath);
    if (RequestedPathLower.rfind("skin=", 0) == 0)
        RequestedPath = RequestedPath.substr(5);

    if (RequestedPath.find('/') != std::string::npos)
    {
        UEAllocatedWString WidePath(RequestedPath.begin(), RequestedPath.end());
        auto Character = PlayerAIValidateCharacterDefinition(PlayerAITryFindLoadedCosmetic(
                WidePath.c_str(), UAthenaCharacterItemDefinition::StaticClass()));

        if (!Character && LoadPolicy == EPlayerAICosmeticLoadPolicy::AllowSynchronousLoad)
        {
            Character = PlayerAIValidateCharacterDefinition(PlayerAITryLoadCosmetic(
                    WidePath.c_str(), UAthenaCharacterItemDefinition::StaticClass()));
        }

        if (Character)
        {
            PlayerAICacheResolvedSkin(Character);
            return Character;
        }
    }

    UEAllocatedWString WideName(RequestedName.begin(), RequestedName.end());
    UEAllocatedWString CanonicalPath = L"/Game/Athena/Items/Cosmetics/Characters/" +
        WideName + L"." + WideName;
    auto Character = PlayerAIValidateCharacterDefinition(PlayerAITryFindLoadedCosmetic(
            CanonicalPath.c_str(), UAthenaCharacterItemDefinition::StaticClass()));

    if (!Character && LoadPolicy == EPlayerAICosmeticLoadPolicy::AllowSynchronousLoad)
    {
        Character = PlayerAIValidateCharacterDefinition(PlayerAITryLoadCosmetic(
                CanonicalPath.c_str(), UAthenaCharacterItemDefinition::StaticClass()));
    }

    if (Character)
        PlayerAICacheResolvedSkin(Character);

    return Character;
}

bool VersionFeatureAdapter::ApplyCharacterSkin(AFortPlayerStateAthena* PlayerState,
    AFortPlayerPawnAthena* Pawn, UAthenaCharacterItemDefinition* Character,
    EPlayerAICosmeticLoadPolicy LoadPolicy, bool* OutTargetRetained)
{
    bPlayerAILastCheatBotVisualCallPerformedNativeWork = false;
    if (OutTargetRetained)
        *OutTargetRetained = false;
    if (!PlayerState || !PlayerAIValidateCharacterDefinition(Character))
    {
        return false;
    }

    auto Hero = Character->HeroDefinition;
    auto HeroClass = UFortHeroType::StaticClass();
    auto PartClass = UCustomCharacterPart::StaticClass();

    if (!HeroClass || !PartClass || !PlayerAIIsLiveSupportObject(Hero) ||
        !reinterpret_cast<const UObject*>(Hero)->IsA(HeroClass))
    {
        return false;
    }

    const UObject* Parts[PlayerAICharacterPartSlotCount] = {};
    const bool bResolvedFromCache = PlayerAITryGetCachedResolvedSkinParts(
            Character, PartClass, Parts);
    bool bHasBaseParts = false;
    bool bAllAuthoredPartsResolved = bResolvedFromCache;
    int SynchronousResolveBudget = 8;

    auto ResolveCosmeticSoftObject = [&](FSoftObjectPtr& SoftObject,
            const UClass* Class) -> const UObject*
        {
            if (auto Resident = PlayerAITryResolveLoadedSoftObject(SoftObject, Class))
            {
                return Resident;
            }

            if (LoadPolicy != EPlayerAICosmeticLoadPolicy::AllowSynchronousLoad ||
                SynchronousResolveBudget <= 0)
            {
                return nullptr;
            }

            SynchronousResolveBudget--;
            return PlayerAITryResolveSoftObjectForCommand(SoftObject, Class);
        };

    if (!bResolvedFromCache)
    {
        bAllAuthoredPartsResolved = true;

        const bool bHasDeclaredBaseParts = Character->HasBaseCharacterParts() &&
            Character->BaseCharacterParts.Num() > 0;
        const bool bHasSafeBaseParts = bHasDeclaredBaseParts && PlayerAIHasSafeSoftObjectArray(
                Character->BaseCharacterParts, 16);
        if (bHasDeclaredBaseParts && !bHasSafeBaseParts)
            bAllAuthoredPartsResolved = false;

        if (bHasSafeBaseParts)
        {
            for (int p = 0;
                 p < Character->BaseCharacterParts.Num(); p++)
            {
                auto& PartSoft = Character->BaseCharacterParts.Get(p, FSoftObjectPtr::Size());
                auto Part = (const UCustomCharacterPart*)
                    ResolveCosmeticSoftObject(PartSoft, PartClass);

                if (!PlayerAIIsLiveSupportObject(Part) || !reinterpret_cast<const UObject*>(Part)->
                        IsA(PartClass) || !Part->HasCharacterPartType())
                {
                    if (Part || PlayerAIGetSoftReferencePathState(PartSoft) !=
                            EPlayerAISoftReferencePathState::Empty)
                    {
                        bAllAuthoredPartsResolved = false;
                    }
                    continue;
                }

                const int Index = Part->CharacterPartType;
                if (Index >= 0 && Index < PlayerAICharacterPartSlotCount)
                {
                    Parts[Index] = (const UObject*)Part;
                    bHasBaseParts = true;
                }
                else
                {
                    bAllAuthoredPartsResolved = false;
                }
            }
        }

        const bool bHasDeclaredSpecializations = Hero->HasSpecializations() &&
            Hero->Specializations.Num() > 0;
        const bool bHasSafeSpecializations = bHasDeclaredSpecializations &&
            PlayerAIHasSafeSoftObjectArray(Hero->Specializations, 64);
        if (bHasDeclaredSpecializations && !bHasSafeSpecializations)
        {
            bAllAuthoredPartsResolved = false;
        }

        if (bHasSafeSpecializations)
        {
            for (int s = 0;
                 s < Hero->Specializations.Num(); s++)
            {
                auto& SpecSoft = Hero->Specializations.Get(s, FSoftObjectPtr::Size());
                auto Spec = (const UFortHeroSpecialization*)
                    ResolveCosmeticSoftObject(SpecSoft, UFortHeroSpecialization::StaticClass());
                auto SpecObject = reinterpret_cast<const UObject*>(Spec);
                auto SpecClass = UFortHeroSpecialization::StaticClass();

                if (!SpecClass || !PlayerAIIsLiveSupportObject(SpecObject) ||
                    !SpecObject->IsA(SpecClass))
                {
                    if (SpecObject || PlayerAIGetSoftReferencePathState(SpecSoft) !=
                            EPlayerAISoftReferencePathState::Empty)
                    {
                        bAllAuthoredPartsResolved = false;
                    }
                    continue;
                }

                const bool bHasDeclaredParts = Spec->HasCharacterParts() &&
                    Spec->CharacterParts.Num() > 0;
                const bool bHasSafeParts = bHasDeclaredParts && PlayerAIHasSafeSoftObjectArray(
                        Spec->CharacterParts, 16);
                if (bHasDeclaredParts && !bHasSafeParts)
                    bAllAuthoredPartsResolved = false;
                if (!bHasSafeParts)
                    continue;

                for (int p = 0;
                     p < Spec->CharacterParts.Num(); p++)
                {
                    auto& PartSoft = Spec->CharacterParts.Get(p, FSoftObjectPtr::Size());
                    auto Part = (const UCustomCharacterPart*)
                        ResolveCosmeticSoftObject(PartSoft, PartClass);

                    if (!PlayerAIIsLiveSupportObject(Part) ||
                        !reinterpret_cast<const UObject*>(Part)->IsA(PartClass) ||
                        !Part->HasCharacterPartType())
                    {
                        if (Part || PlayerAIGetSoftReferencePathState(PartSoft) !=
                                EPlayerAISoftReferencePathState::Empty)
                        {
                            bAllAuthoredPartsResolved = false;
                        }
                        continue;
                    }

                    const int Index = Part->CharacterPartType;
                    if (Index >= 0 && Index < PlayerAICharacterPartSlotCount &&
                        (!bHasBaseParts || !Parts[Index]))
                    {
                        Parts[Index] = (const UObject*)Part;
                    }
                    else if (Index < 0 || Index >= PlayerAICharacterPartSlotCount)
                    {
                        bAllAuthoredPartsResolved = false;
                    }
                }
            }
        }
    }

    if (!bAllAuthoredPartsResolved || !PlayerAIIsLiveSupportObject(Parts[0]) ||
        !PlayerAIIsLiveSupportObject(Parts[1]))
    {
        return false;
    }

    if (!bResolvedFromCache)
        PlayerAICacheResolvedSkinParts(Character, Parts, bAllAuthoredPartsResolved);

    if (PlayerAIIsCheatBotVisualCommitPending(Pawn, Character))
    {
        bool bTargetRetained = false;
        if (!PlayerAIFinishRequestedCheatBotCosmetics(PlayerState, Pawn, Parts, Character, nullptr,
                &bTargetRetained))
        {
            if (OutTargetRetained)
                *OutTargetRetained = bTargetRetained;
            return false;
        }
        PlayerAICacheResolvedSkin(Character);
        return true;
    }

    FPlayerAICosmeticMetadataSnapshot PreviousMetadata;
    PreviousMetadata.PlayerState = TWeakObjectPtr<AFortPlayerStateAthena>(PlayerState);
    PreviousMetadata.bHadHeroTypeProperty = PlayerState->HasHeroType();
    PreviousMetadata.bHadHeroTypeValue = PreviousMetadata.bHadHeroTypeProperty &&
        PlayerState->HeroType != nullptr;
    if (PreviousMetadata.bHadHeroTypeProperty && PlayerState->HeroType)
    {
        PreviousMetadata.HeroType = TWeakObjectPtr<UObject>(const_cast<UObject*>(
                reinterpret_cast<const UObject*>(PlayerState->HeroType)));
    }

    EFortCustomGender SelectedGender = EFortCustomGender::Invalid;
    const bool bWriteGender = PlayerAIResolveConcreteGender(
            PlayerState, Character, Parts, SelectedGender);
    PreviousMetadata.ReplicatedGender = PlayerAICaptureEnumByte(PlayerState, "Gender", L"Gender");
    if (!PreviousMetadata.ReplicatedGender.bValid)
    {
        PreviousMetadata.ReplicatedGender = PlayerAICaptureEnumByte(PlayerState,
                "CharacterGender", L"CharacterGender");
    }
    PreviousMetadata.LocalGender = PlayerAICaptureEnumByte(
        PlayerState, "LocalCharacterGender", nullptr);

    uint8 SelectedBodyType = 0;
    const bool bWriteBodyType = PlayerAIResolveConcreteBodyType(
            PlayerState, Parts, SelectedBodyType);
    PreviousMetadata.ReplicatedBodyType = PlayerAICaptureEnumByte(PlayerState,
            "CharacterBodyType", L"CharacterBodyType");
    if (!PreviousMetadata.ReplicatedBodyType.bValid)
    {
        PreviousMetadata.ReplicatedBodyType = PlayerAICaptureEnumByte(
                PlayerState, "BodyType", L"BodyType");
    }
    PreviousMetadata.LocalBodyType = PlayerAICaptureEnumByte(
        PlayerState, "LocalCharacterBodyType", nullptr);

    const bool bStageCheatBotMetadata = VersionInfo.FortniteVersion >= 19.0 && Pawn &&
        PendingBotSkinCommitPawnCounts.contains(Pawn);
    if (!bStageCheatBotMetadata)
    {
        PlayerAIWriteHeroType(PlayerState, (const UObject*)Hero, true);
    }

    if (bWriteGender && !bStageCheatBotMetadata)
        PlayerAIWriteGender(PlayerState, SelectedGender, true);
    if (bWriteBodyType && !bStageCheatBotMetadata)
        PlayerAIWriteBodyType(PlayerState, SelectedBodyType, true);

    bool bTargetRetained = false;
    if (!PlayerAIFinishRequestedCheatBotCosmetics(PlayerState, Pawn, Parts, Character,
            &PreviousMetadata, &bTargetRetained))
    {
        if (!bTargetRetained && !PlayerAIIsCheatBotVisualCommitPending(Pawn, Character))
        {
            PlayerAIRestoreCosmeticMetadata(PreviousMetadata);
        }
        if (OutTargetRetained)
            *OutTargetRetained = bTargetRetained;
        return false;
    }

    PlayerAICacheResolvedSkin(Character);
    return true;
}

UAthenaCharacterItemDefinition* VersionFeatureAdapter::ApplyRandomSkin(
    AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn,
    EPlayerAICosmeticLoadPolicy LoadPolicy)
{
    if (!PlayerState)
        return nullptr;

    const bool bAllowSynchronousLoad = LoadPolicy ==
        EPlayerAICosmeticLoadPolicy::AllowSynchronousLoad;

    constexpr int MaxResidentCandidates = 5;
    int Attempts = 0;
    if (!CachedSkins.empty())
    {
        const int ResidentAttempts = (std::min)((int)CachedSkins.size(), MaxResidentCandidates);
        const int Start = rand() % CachedSkins.size();

        for (int Offset = 0;
             Offset < ResidentAttempts; Offset++)
        {
            Attempts++;
            auto Character = CachedSkins[(Start + Offset) % CachedSkins.size()];

            if (ApplyCharacterSkin(PlayerState, Pawn, Character,
                    EPlayerAICosmeticLoadPolicy::ResidentOnly))
            {
                AIDebugLogger::Verbose("Cosmetics", "applied resident skin %s",
                    Character->Name.ToString().c_str());
                return Character;
            }
        }
    }

    constexpr int MaxSynchronousCandidates = 1;
    int SynchronousAttempts = 0;

    if (bAllowSynchronousLoad && !CachedSkinCatalog.empty())
    {
        const int CatalogCount = (int)CachedSkinCatalog.size();
        const int CatalogAttempts = (std::min)(CatalogCount, MaxSynchronousCandidates);
        const int Start = rand() % CatalogCount;

        for (int Offset = 0;
             Offset < CatalogAttempts; Offset++)
        {
            Attempts++;
            SynchronousAttempts++;
            const auto& Entry = CachedSkinCatalog[(Start + Offset) % CatalogCount];
            auto Character = PlayerAIValidateCharacterDefinition(PlayerAITryResolveCatalogEntry(
                        Entry, LoadPolicy));

            if (Character && ApplyCharacterSkin(PlayerState, Pawn, Character, LoadPolicy))
            {
                PlayerAICacheResolvedSkin(Character);
                AIDebugLogger::Verbose("Cosmetics", "applied catalog skin %s", Entry.Name.c_str());
                return Character;
            }
        }
    }

    // 1.7.2 predates the primary-asset Blueprint catalog, so probe the stable early CID set instead.
    if (bAllowSynchronousLoad && SynchronousAttempts < MaxSynchronousCandidates)
    {
        const int KnownCount = (int)(sizeof(PlayerAIKnownSkinNames) /
                sizeof(PlayerAIKnownSkinNames[0]));
        const int CandidateCount = VersionInfo.FortniteVersion < 2.00 ? (std::min)(KnownCount, 8)
            : KnownCount;
        const int KnownAttempts = (std::min)(CandidateCount, MaxSynchronousCandidates -
                SynchronousAttempts);
        const int Start = rand() % CandidateCount;

        for (int Offset = 0;
             Offset < KnownAttempts; Offset++)
        {
            Attempts++;
            SynchronousAttempts++;
            auto WideName = PlayerAIKnownSkinNames[(Start + Offset) % CandidateCount];
            UEAllocatedWString Path = UEAllocatedWString(
                    L"/Game/Athena/Items/Cosmetics/Characters/") + WideName + L"." + WideName;
            auto Character = PlayerAIValidateCharacterDefinition(PlayerAITryLoadCosmetic(
                        Path.c_str(), UAthenaCharacterItemDefinition::StaticClass()));

            if (Character && ApplyCharacterSkin(PlayerState, Pawn, Character, LoadPolicy))
            {
                return Character;
            }
        }
    }

    AIDebugLogger::Log("Cosmetics",
        "random skin resolution failed after %d candidates - using the default outfit", Attempts);
    ApplyDefaultCosmetics(PlayerState, Pawn, SynchronousAttempts > 0
            ? EPlayerAICosmeticLoadPolicy::ResidentOnly : LoadPolicy);
    return nullptr;
}

static bool PlayerAIChooseQueuedCatalogSkin(FPlayerAISkinCatalogEntry& OutEntry)
{
    const size_t CatalogSize = CachedSkinCatalog.size();
    if (!bSkinCatalogReady || CatalogSize == 0)
        return false;

    if (RandomSkinCatalogShuffleSize != CatalogSize ||
        RandomSkinCatalogShuffleCursor >= CatalogSize)
    {
        RandomSkinCatalogShuffleStart = static_cast<size_t>(rand()) % CatalogSize;

        if (CatalogSize > 1 && !LastQueuedRandomSkinName.empty() && CachedSkinCatalog[
                RandomSkinCatalogShuffleStart].Name == LastQueuedRandomSkinName)
        {
            RandomSkinCatalogShuffleStart = (RandomSkinCatalogShuffleStart + 1) % CatalogSize;
        }

        RandomSkinCatalogShuffleCursor = 0;
        RandomSkinCatalogShuffleSize = CatalogSize;
    }

    const size_t CatalogIndex = (RandomSkinCatalogShuffleStart +
         RandomSkinCatalogShuffleCursor++) % CatalogSize;
    if (CatalogIndex >= CachedSkinCatalog.size())
        return false;

    OutEntry = CachedSkinCatalog[CatalogIndex];
    LastQueuedRandomSkinName = OutEntry.Name;
    return OutEntry.PrimaryAssetIdSize > 0;
}

static UAthenaCharacterItemDefinition* PlayerAIChooseQueuedResidentSkin(
    FPlayerAIPendingRandomSkin& Pending)
{
    const size_t SkinCount = CachedSkins.size();
    if (SkinCount == 0)
        return nullptr;

    if (!Pending.bResidentSelectionInitialized || Pending.ResidentSelectionStart >= SkinCount)
    {
        Pending.ResidentSelectionStart = static_cast<size_t>(rand()) % SkinCount;
        Pending.ResidentSelectionCursor = 0;
        Pending.bResidentSelectionInitialized = true;
    }

    const size_t ProbeCount = (std::min)(SkinCount, size_t(4));
    for (size_t Probe = 0; Probe < ProbeCount; Probe++)
    {
        const size_t Index = (Pending.ResidentSelectionStart +
             Pending.ResidentSelectionCursor++) % SkinCount;
        auto Character = PlayerAIValidateCharacterDefinition(CachedSkins[Index]);
        if (!Character)
            continue;

        auto RawName = Character->Name.ToString();
        std::string Name(RawName.c_str());
        if (SkinCount > 1 && Name == LastQueuedRandomSkinName)
        {
            continue;
        }

        LastQueuedRandomSkinName = Name;
        return Character;
    }

    return nullptr;
}

static PlayerLoadout::FSoftObjectLoadResult
PlayerAIResolveOrRequestQueuedCatalogSkin(const FPlayerAISkinCatalogEntry& Entry,
    AFortPlayerPawnAthena* Pawn)
{
    PlayerLoadout::FSoftObjectLoadResult Result{
        nullptr, PlayerLoadout::EPreviewTextureLoadState::Unavailable, 0,
    };

    if (!Pawn || Entry.PrimaryAssetIdSize <= 0 || Entry.PrimaryAssetIdSize >
            (int32)sizeof(Entry.RawPrimaryAssetId) || bPrimaryAssetCosmeticFunctionsDisabled)
    {
        return Result;
    }

    if (auto Character = PlayerAITryResolveCatalogEntry(
            Entry, EPlayerAICosmeticLoadPolicy::ResidentOnly))
    {
        Result.Object = Character;
        Result.State = PlayerLoadout::EPreviewTextureLoadState::Resident;
        return Result;
    }

    auto SystemLibrary = UKismetSystemLibrary::GetDefaultObj();
    auto CharacterClass = UAthenaCharacterItemDefinition::StaticClass();
    auto Function = SystemLibrary ? SystemLibrary->GetFunction(
            "GetSoftObjectReferenceFromPrimaryAssetId") : nullptr;
    if (!SystemLibrary || !CharacterClass || !Function)
        return Result;

    const uint32 InputOffset = Function->GetOffset("PrimaryAssetId");
    const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
    const size_t ParamsSize = PlayerAICosmeticFunctionParamSize(Function);
    const size_t SoftObjectSize = FSoftObjectPtr::Size();
    const size_t IdSize = static_cast<size_t>(Entry.PrimaryAssetIdSize);
    const bool bSchemaValid = InputOffset == 0 && ReturnOffset == IdSize &&
        PlayerAIParameterRangeFits(ParamsSize, InputOffset, IdSize) && PlayerAIParameterRangeFits(
            ParamsSize, ReturnOffset, SoftObjectSize);
    if (!bSchemaValid)
        return Result;

    auto Params = FMemory::Malloc(ParamsSize);
    if (!Params)
        return Result;

    memset(Params, 0, ParamsSize);
    memcpy((uint8*)Params + InputOffset, Entry.RawPrimaryAssetId, Entry.PrimaryAssetIdSize);
    const bool bCalled = PlayerAITryProcessCosmeticFunction(SystemLibrary, Function, Params);

    if (bCalled)
    {
        auto SoftObject = reinterpret_cast<FSoftObjectPtr*>((uint8*)Params + ReturnOffset);
        Result = PlayerLoadout::ResolveOrRequestSoftObject(Pawn, SoftObject,
            static_cast<uint32>(SoftObjectSize), CharacterClass);
        PlayerAIFreeReturnedSoftReferencePath(SoftObject);
    }
    else
    {
        bPrimaryAssetCosmeticFunctionsDisabled = true;
    }

    FMemory::Free(Params);
    if (Result.Object && !PlayerAIValidateCharacterDefinition(Result.Object))
    {
        Result.Object = nullptr;
        Result.State = PlayerLoadout::EPreviewTextureLoadState::Unavailable;
    }

    return Result;
}

static void PlayerAIResetPendingRandomSkinCandidate(FPlayerAIPendingRandomSkin& Pending)
{
    Pending.CatalogEntry = {};
    Pending.Character = {};
    Pending.Specialization = {};
    for (auto& Part : Pending.ResolvedParts)
        Part = {};
    Pending.BasePartCursor = 0;
    Pending.SpecializationCursor = 0;
    Pending.SpecializationPartCursor = 0;
    Pending.PartReferencesVisited = 0;
    Pending.CandidateStartedAt = 0;
    Pending.bHasBaseParts = false;
    Pending.ResolvedPartMask = 0;
    Pending.bHasHead = false;
    Pending.bHasBody = false;
    Pending.Phase = EPlayerAIPendingRandomSkinPhase::ResolveCharacter;
}

static bool PlayerAIObserveQueuedCharacterPart(FPlayerAIPendingRandomSkin& Pending,
    const UObject* Object, const UClass* PartClass, bool bFromBaseParts)
{
    if (!PartClass || !PlayerAIIsLiveSupportObject(Object) || !Object->IsA(PartClass))
    {
        return false;
    }

    auto Part = static_cast<const UCustomCharacterPart*>(Object);
    if (!Part->HasCharacterPartType())
        return false;

    const int PartType = Part->CharacterPartType;
    if (PartType < 0 || PartType >= PlayerAICharacterPartSlotCount)
        return false;

    if (bFromBaseParts)
    {
        Pending.bHasBaseParts = true;
        Pending.ResolvedParts[PartType] = TWeakObjectPtr<UObject>(const_cast<UObject*>(Object));
    }
    else if (!Pending.bHasBaseParts || (Pending.ResolvedPartMask &
            static_cast<uint8>(1u << PartType)) == 0)
    {
        Pending.ResolvedParts[PartType] = TWeakObjectPtr<UObject>(const_cast<UObject*>(Object));
    }

    Pending.bHasHead |= PartType == 0;
    Pending.bHasBody |= PartType == 1;
    Pending.ResolvedPartMask |= static_cast<uint8>(1u << PartType);
    return true;
}

enum class EPlayerAIQueuedPartAdvanceResult : uint8
{
    Keep, Ready, RejectCandidate,
};

static EPlayerAIQueuedPartAdvanceResult
PlayerAIAdvanceQueuedCharacterParts(FPlayerAIPendingRandomSkin& Pending,
    UAthenaCharacterItemDefinition* Character, ULONGLONG Now)
{
    auto PartClass = UCustomCharacterPart::StaticClass();
    auto SpecClass = UFortHeroSpecialization::StaticClass();
    auto HeroClass = UFortHeroType::StaticClass();
    auto Hero = Character ? Character->HeroDefinition : nullptr;
    if (!PlayerAIValidateCharacterDefinition(Character) || !PartClass || !SpecClass || !HeroClass ||
        !PlayerAIIsLiveSupportObject(Hero) ||
        !reinterpret_cast<const UObject*>(Hero)->IsA(HeroClass))
    {
        return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
    }

    int ResolveBudget = PlayerAIRandomSkinSoftResolvesPerRecordTick;
    while (ResolveBudget > 0)
    {
        if (Pending.Phase == EPlayerAIPendingRandomSkinPhase::BaseParts)
        {
            const bool bHasBasePartsProperty = Character->HasBaseCharacterParts();
            const int BasePartCount = bHasBasePartsProperty ? Character->BaseCharacterParts.Num()
                : 0;
            if (!bHasBasePartsProperty || BasePartCount == 0)
            {
                Pending.Phase = EPlayerAIPendingRandomSkinPhase::Specializations;
                continue;
            }
            if (BasePartCount < 0 || !PlayerAIHasSafeSoftObjectArray(
                    Character->BaseCharacterParts, 16))
            {
                return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
            }
            if (Pending.BasePartCursor >= BasePartCount)
            {
                Pending.Phase = EPlayerAIPendingRandomSkinPhase::Specializations;
                continue;
            }
            if (Pending.PartReferencesVisited >= PlayerAIRandomSkinMaxPartReferences)
            {
                return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
            }

            auto& SoftPart = Character->BaseCharacterParts.Get(
                Pending.BasePartCursor, FSoftObjectPtr::Size());
            auto PartResult = PlayerLoadout::ResolveOrRequestSoftObject(
                    Character, &SoftPart, FSoftObjectPtr::Size(), PartClass);
            ResolveBudget--;
            if (PartResult.State == PlayerLoadout::EPreviewTextureLoadState::Pending)
            {
                Pending.RetryAt = Now + (std::max)(static_cast<ULONGLONG>(50),
                    static_cast<ULONGLONG>(PartResult.RetryAfterMs));
                return EPlayerAIQueuedPartAdvanceResult::Keep;
            }

            if (!PlayerAIObserveQueuedCharacterPart(Pending, PartResult.Object, PartClass, true))
            {
                if (PartResult.Object || PlayerAIGetSoftReferencePathState(SoftPart) !=
                    EPlayerAISoftReferencePathState::Empty)
                {
                    return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
                }
            }

            Pending.BasePartCursor++;
            Pending.PartReferencesVisited++;
            Pending.CandidateStartedAt = Now;
            continue;
        }

        if (Pending.Phase == EPlayerAIPendingRandomSkinPhase::Specializations)
        {
            const bool bHasSpecializationsProperty = Hero->HasSpecializations();
            const int SpecializationCount = bHasSpecializationsProperty
                    ? Hero->Specializations.Num() : 0;
            if (!bHasSpecializationsProperty || SpecializationCount == 0)
            {
                return Pending.bHasHead && Pending.bHasBody
                    ? EPlayerAIQueuedPartAdvanceResult::Ready
                    : EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
            }
            if (SpecializationCount < 0 || !PlayerAIHasSafeSoftObjectArray(
                    Hero->Specializations, 64))
            {
                return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
            }
            if (Pending.SpecializationCursor >= SpecializationCount)
            {
                if (!Pending.bHasHead || !Pending.bHasBody)
                    return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;

                Pending.Phase = EPlayerAIPendingRandomSkinPhase::Ready;
                continue;
            }

            auto& SoftSpec = Hero->Specializations.Get(Pending.SpecializationCursor,
                FSoftObjectPtr::Size());
            auto SpecResult = PlayerLoadout::ResolveOrRequestSoftObject(
                    Hero, &SoftSpec, FSoftObjectPtr::Size(), SpecClass);
            ResolveBudget--;
            if (SpecResult.State == PlayerLoadout::EPreviewTextureLoadState::Pending)
            {
                Pending.RetryAt = Now + (std::max)(static_cast<ULONGLONG>(50),
                    static_cast<ULONGLONG>(SpecResult.RetryAfterMs));
                return EPlayerAIQueuedPartAdvanceResult::Keep;
            }

            auto SpecObject = SpecResult.Object;
            if (!PlayerAIIsLiveSupportObject(SpecObject) || !SpecObject->IsA(SpecClass))
            {
                if (SpecObject || PlayerAIGetSoftReferencePathState(SoftSpec) !=
                        EPlayerAISoftReferencePathState::Empty)
                {
                    return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
                }
                Pending.SpecializationCursor++;
                Pending.CandidateStartedAt = Now;
                continue;
            }

            Pending.Specialization = TWeakObjectPtr<UObject>(const_cast<UObject*>(SpecObject));
            Pending.SpecializationPartCursor = 0;
            Pending.CandidateStartedAt = Now;
            Pending.Phase = EPlayerAIPendingRandomSkinPhase::SpecializationParts;
            continue;
        }

        if (Pending.Phase == EPlayerAIPendingRandomSkinPhase::SpecializationParts)
        {
            auto SpecObject = Pending.Specialization.Get();
            if (!PlayerAIIsLiveSupportObject(SpecObject) || !SpecObject->IsA(SpecClass))
            {
                return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
            }

            auto Spec = reinterpret_cast<const UFortHeroSpecialization*>(SpecObject);
            const bool bHasCharacterPartsProperty = Spec->HasCharacterParts();
            const int CharacterPartCount = bHasCharacterPartsProperty ? Spec->CharacterParts.Num()
                    : 0;
            if (!bHasCharacterPartsProperty || CharacterPartCount == 0)
            {
                Pending.Specialization = {};
                Pending.SpecializationCursor++;
                Pending.SpecializationPartCursor = 0;
                Pending.CandidateStartedAt = Now;
                Pending.Phase = EPlayerAIPendingRandomSkinPhase::Specializations;
                continue;
            }
            if (CharacterPartCount < 0 || !PlayerAIHasSafeSoftObjectArray(Spec->CharacterParts, 16))
            {
                return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
            }
            if (Pending.SpecializationPartCursor >= CharacterPartCount)
            {
                Pending.Specialization = {};
                Pending.SpecializationCursor++;
                Pending.SpecializationPartCursor = 0;
                Pending.CandidateStartedAt = Now;
                Pending.Phase = EPlayerAIPendingRandomSkinPhase::Specializations;
                continue;
            }
            if (Pending.PartReferencesVisited >= PlayerAIRandomSkinMaxPartReferences)
            {
                return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
            }

            auto& SoftPart = Spec->CharacterParts.Get(Pending.SpecializationPartCursor,
                FSoftObjectPtr::Size());
            auto PartResult = PlayerLoadout::ResolveOrRequestSoftObject(
                    Spec, &SoftPart, FSoftObjectPtr::Size(), PartClass);
            ResolveBudget--;
            if (PartResult.State == PlayerLoadout::EPreviewTextureLoadState::Pending)
            {
                Pending.RetryAt = Now + (std::max)(static_cast<ULONGLONG>(50),
                    static_cast<ULONGLONG>(PartResult.RetryAfterMs));
                return EPlayerAIQueuedPartAdvanceResult::Keep;
            }

            if (!PlayerAIObserveQueuedCharacterPart(Pending, PartResult.Object, PartClass, false))
            {
                if (PartResult.Object || PlayerAIGetSoftReferencePathState(SoftPart) !=
                        EPlayerAISoftReferencePathState::Empty)
                {
                    return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
                }
            }

            Pending.SpecializationPartCursor++;
            Pending.PartReferencesVisited++;
            Pending.CandidateStartedAt = Now;
            continue;
        }

        if (Pending.Phase == EPlayerAIPendingRandomSkinPhase::Ready)
        {
            return Pending.bHasHead && Pending.bHasBody ? EPlayerAIQueuedPartAdvanceResult::Ready
                : EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
        }

        return EPlayerAIQueuedPartAdvanceResult::RejectCandidate;
    }

    Pending.RetryAt = Now + 1;
    return Pending.Phase == EPlayerAIPendingRandomSkinPhase::Ready &&
        Pending.bHasHead && Pending.bHasBody ? EPlayerAIQueuedPartAdvanceResult::Ready
        : EPlayerAIQueuedPartAdvanceResult::Keep;
}

static bool PlayerAICacheCompletedQueuedSkinParts(FPlayerAIPendingRandomSkin& Pending,
    UAthenaCharacterItemDefinition* Character)
{
    auto PartClass = UCustomCharacterPart::StaticClass();
    if (!Character || !PartClass || !Pending.bHasHead || !Pending.bHasBody)
    {
        return false;
    }

    const UObject* Parts[PlayerAICharacterPartSlotCount]{};
    for (int Index = 0;
         Index < PlayerAICharacterPartSlotCount; Index++)
    {
        const bool bWasResolved = (Pending.ResolvedPartMask & static_cast<uint8>(1u << Index)) != 0;
        auto PartObject = Pending.ResolvedParts[Index].Get();
        if (!bWasResolved)
        {
            if (PartObject)
                return false;
            continue;
        }

        if (!PlayerAIIsLiveSupportObject(PartObject) || !PartObject->IsA(PartClass))
        {
            return false;
        }

        auto Part = static_cast<const UCustomCharacterPart*>(PartObject);
        if (!Part->HasCharacterPartType() || Part->CharacterPartType != Index)
        {
            return false;
        }

        Parts[Index] = PartObject;
    }

    if (!Parts[0] || !Parts[1])
        return false;

    PlayerAICacheResolvedSkinParts(Character, Parts, true);
    return true;
}

static UAthenaCharacterItemDefinition* PlayerAITryChooseCachedResolvedRandomSkin()
{
    auto PartClass = UCustomCharacterPart::StaticClass();
    if (!PartClass || CachedResolvedSkinPool.empty())
        return nullptr;

    constexpr int MaxCandidates = 8;
    const int Attempts = (std::min)(static_cast<int>(CachedResolvedSkinPool.size()), MaxCandidates);
    const size_t Start = static_cast<size_t>(rand()) % CachedResolvedSkinPool.size();
    for (int Offset = 0; Offset < Attempts; Offset++)
    {
        auto Character = CachedResolvedSkinPool[(Start + static_cast<size_t>(Offset)) %
                CachedResolvedSkinPool.size()];
        const UObject* Parts[PlayerAICharacterPartSlotCount]{};
        if (!PlayerAIValidateCharacterDefinition(Character) ||
            !PlayerAITryGetCachedResolvedSkinParts(Character, PartClass, Parts))
        {
            continue;
        }

        auto RawName = Character->Name.ToString();
        std::string Name(RawName.c_str());
        if (CachedResolvedSkinPool.size() > 1 && Name == LastQueuedRandomSkinName)
        {
            continue;
        }

        LastQueuedRandomSkinName = Name;
        return Character;
    }

    return nullptr;
}

static UAthenaCharacterItemDefinition* PlayerAITryApplyCachedResolvedRandomSkin(
    AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    auto Character = PlayerAITryChooseCachedResolvedRandomSkin();
    return Character && PlayerState && Pawn && VersionFeatureAdapter::ApplyCharacterSkin(
            PlayerState, Pawn, Character, EPlayerAICosmeticLoadPolicy::ResidentOnly) ? Character
        : nullptr;
}

static bool PlayerAITickPendingRequestedBotSkinCommits()
{
    constexpr int InvalidRecordsPerTick = 4;
    const ULONGLONG Now = GetTickCount64();
    int Inspected = 0;

    while (!PendingRequestedBotSkinCommits.empty() && Inspected < InvalidRecordsPerTick)
    {
        auto& Pending = PendingRequestedBotSkinCommits.front();
        auto World = Pending.World.Get();
        auto Controller = Pending.Controller.Get();
        auto PlayerState = Pending.PlayerState.Get();
        auto Pawn = Pending.Pawn.Get();
        auto RequestedCharacter = Pending.bUseDefault ? nullptr
            : PlayerAIValidateCharacterDefinition(Pending.Character.Get());
        const bool bVisualCommitOwnsRequest = RequestedCharacter && Pawn &&
            PlayerAIIsCheatBotVisualCommitPending(Pawn, RequestedCharacter);
        const bool bValid = World && World == UWorld::GetWorld() &&
            PlayerAIIsLiveSupportObject(Controller) && PlayerAIIsLiveSupportObject(PlayerState) &&
            PlayerAIIsLiveSupportObject(Pawn) && Controller->PlayerState == PlayerState &&
            Pawn->PlayerState == PlayerState && Controller->Pawn == Pawn &&
            (!Controller->HasMyFortPawn() || Controller->MyFortPawn == Pawn) &&
            (!Pawn->HasController() || Pawn->Controller == Controller) &&
            (!Pawn->HasbActorIsBeingDestroyed() || !Pawn->bActorIsBeingDestroyed) &&
            (!Pawn->HasbIsDying() || !Pawn->bIsDying) && (!Pawn->HasbIsDBNO() || !Pawn->bIsDBNO) &&
            (bVisualCommitOwnsRequest || !Pending.WorkStartedAt || Now - Pending.WorkStartedAt <
                PlayerAIRandomSkinLifetimeMs);
        if (!bValid)
        {
            PlayerAIReleasePendingSkinCommit(Pending.PawnIdentity);
            PendingRequestedBotSkinCommits.erase(PendingRequestedBotSkinCommits.begin());
            Inspected++;
            continue;
        }
        if (Pending.RetryAt > Now)
        {
            std::rotate(PendingRequestedBotSkinCommits.begin(),
                PendingRequestedBotSkinCommits.begin() + 1, PendingRequestedBotSkinCommits.end());
            Inspected++;
            continue;
        }

        if (VersionInfo.FortniteVersion >= 19.0 && (!Pawn->HasController() ||
             Pawn->Controller != Controller || !AFortPlayerControllerAthena::
                IsCheatSpawnedBotController(Controller)))
        {
            Pending.RetryAt = Now + 50ULL;
            std::rotate(PendingRequestedBotSkinCommits.begin(),
                PendingRequestedBotSkinCommits.begin() + 1, PendingRequestedBotSkinCommits.end());
            Inspected++;
            continue;
        }

        if (VersionInfo.FortniteVersion >= 19.0)
        {
            const bool bAnotherVisualCommitActive = std::any_of(
                    PendingCheatBotVisualCommits.begin(), PendingCheatBotVisualCommits.end(),
                    [Pawn](const auto& Pair)
                    {
                        return Pair.first != Pawn;
                    });
            if (bAnotherVisualCommitActive)
            {
                Pending.RetryAt = Now + 100ULL;
                std::rotate(PendingRequestedBotSkinCommits.begin(),
                    PendingRequestedBotSkinCommits.begin() + 1,
                    PendingRequestedBotSkinCommits.end());
                Inspected++;
                continue;
            }
        }

        auto Character = RequestedCharacter;
        if (!Pending.WorkStartedAt)
        {
            Pending.WorkStartedAt = Now;
            PlayerAIMarkPendingSkinCommitActive(Pawn);
        }
        bool bTargetRetained = false;
        const bool bAppliedCharacter = Character && VersionFeatureAdapter::ApplyCharacterSkin(
                PlayerState, Pawn, Character, EPlayerAICosmeticLoadPolicy::AllowSynchronousLoad,
                &bTargetRetained);
        bool bAppliedDefault = false;

        if (bTargetRetained)
        {
            AFortPlayerPawnAthena::EnsurePlayerMapIcon(Controller, Pawn, Character);
            PlayerAIReleasePendingSkinCommit(Pending.PawnIdentity);
            PendingRequestedBotSkinCommits.erase(PendingRequestedBotSkinCommits.begin());
            return true;
        }

        if (!bAppliedCharacter)
        {
            if (Character)
            {
                if (PlayerAIIsCheatBotVisualCommitPending(Pawn, Character))
                {
                    Pending.RetryAt = Now + 50ULL;
                    std::rotate(PendingRequestedBotSkinCommits.begin(),
                        PendingRequestedBotSkinCommits.begin() + 1,
                        PendingRequestedBotSkinCommits.end());
                    if (bPlayerAILastCheatBotVisualCallPerformedNativeWork)
                        return true;
                    Inspected++;
                    continue;
                }

                const UObject* CachedParts[PlayerAICharacterPartSlotCount]{};
                const bool bAllPartsResolved = PlayerAITryGetCachedResolvedSkinParts(Character,
                        UCustomCharacterPart::StaticClass(), CachedParts);
                if (!bAllPartsResolved)
                {
                    Pending.RetryAt = Now + 100ULL;
                    std::rotate(PendingRequestedBotSkinCommits.begin(),
                        PendingRequestedBotSkinCommits.begin() + 1,
                        PendingRequestedBotSkinCommits.end());
                    return true;
                }

                Pending.Character = {};
                Pending.bUseDefault = true;
                Pending.RetryAt = Now + 50ULL;
                std::rotate(PendingRequestedBotSkinCommits.begin(),
                    PendingRequestedBotSkinCommits.begin() + 1,
                    PendingRequestedBotSkinCommits.end());
                return true;
            }

            bAppliedDefault = true;
        }

        if (!bAppliedCharacter && !bAppliedDefault)
        {
            auto PawnIdentity = Pending.PawnIdentity;
            PlayerAIReleasePendingSkinCommit(PawnIdentity);
            PendingRequestedBotSkinCommits.erase(PendingRequestedBotSkinCommits.begin());
            return true;
        }

        AFortPlayerPawnAthena::EnsurePlayerMapIcon(Controller, Pawn,
            bAppliedCharacter ? Character : nullptr);
        PlayerAIReleasePendingSkinCommit(Pending.PawnIdentity);
        PendingRequestedBotSkinCommits.erase(PendingRequestedBotSkinCommits.begin());
        return true;
    }

    return false;
}

enum class EPlayerAIPendingRandomSkinResult : uint8
{
    Keep, KeepAfterCosmeticAttempt, Applied, DropAfterCosmeticAttempt, Drop,
};

static int PlayerAICountUnresolvedRandomSkinCandidates()
{
    int Count = 0;
    for (const auto& Other : PendingRandomBotSkins)
    {
        auto Character = PlayerAIValidateCharacterDefinition(Other.Character.Get());
        if (Character && PlayerAIIsCheatBotVisualCommitPending(Other.Pawn.Get(), Character))
        {
            Count++;
            continue;
        }
        if (!Other.CandidateStartedAt || Other.Phase == EPlayerAIPendingRandomSkinPhase::Ready)
        {
            continue;
        }
        Count++;
    }
    return (std::max)(Count, static_cast<int>(PendingCheatBotVisualCommits.size()));
}

static EPlayerAIPendingRandomSkinResult
PlayerAITickPendingRandomBotSkin(FPlayerAIPendingRandomSkin& Pending, ULONGLONG Now)
{
    auto World = Pending.World.Get();
    auto Controller = Pending.Controller.Get();
    auto PlayerState = Pending.PlayerState.Get();
    auto Pawn = Pending.Pawn.Get();
    if (!World || World != UWorld::GetWorld() || !PlayerAIIsLiveSupportObject(Controller) ||
        !PlayerAIIsLiveSupportObject(PlayerState) || !PlayerAIIsLiveSupportObject(Pawn) ||
        Controller->PlayerState != PlayerState || Pawn->PlayerState != PlayerState ||
        Controller->Pawn != Pawn || (Controller->HasMyFortPawn() &&
         Controller->MyFortPawn != Pawn) ||
        (Pawn->HasController() && Pawn->Controller != Controller) ||
        (Pawn->HasbActorIsBeingDestroyed() && Pawn->bActorIsBeingDestroyed) ||
        (Pawn->HasbIsDying() && Pawn->bIsDying) || (Pawn->HasbIsDBNO() && Pawn->bIsDBNO))
    {
        return EPlayerAIPendingRandomSkinResult::Drop;
    }

    if (VersionInfo.FortniteVersion >= 19.0 && (!Pawn->HasController() ||
         Pawn->Controller != Controller || !AFortPlayerControllerAthena::
            IsCheatSpawnedBotController(Controller)))
    {
        Pending.RetryAt = Now + 50ULL;
        return EPlayerAIPendingRandomSkinResult::Keep;
    }

    if (bPrimaryAssetCosmeticFunctionsDisabled)
    {
        Pending.bUseResidentFallback = true;
    }

    if (!Pending.bUseResidentFallback && !bSkinCatalogReady)
    {
        const bool bCatalogWaitExpired = Now - Pending.QueuedAt >=
                PlayerAIRandomSkinCatalogWaitMs && !PendingSkinCatalogData;
        const bool bCatalogFailed = SkinCatalogAttempts >= 20 && !PendingSkinCatalogData;
        if (bCatalogWaitExpired || bCatalogFailed)
        {
            Pending.bUseResidentFallback = true;
            Pending.RetryAt = 0;
        }
        else
        {
            Pending.RetryAt = Now + 50ULL;
            return EPlayerAIPendingRandomSkinResult::Keep;
        }
    }

    const bool bVisualCommitOwnsCandidate = PlayerAIIsCheatBotVisualCommitPending(Pawn,
            PlayerAIValidateCharacterDefinition(Pending.Character.Get()));
    if (Pending.WorkStartedAt && !bVisualCommitOwnsCandidate && Now - Pending.WorkStartedAt >=
            PlayerAIRandomSkinLifetimeMs)
    {
        AIDebugLogger::Log("Cosmetics",
            "deferred random skin expired for pawn=%p after %d candidates; keeping the engine default",
            (void*)Pawn, Pending.CandidateAttempts);
        return EPlayerAIPendingRandomSkinResult::Drop;
    }

    if (Pending.CandidateStartedAt && !bVisualCommitOwnsCandidate &&
        Now - Pending.CandidateStartedAt >= PlayerAIRandomSkinCandidateLifetimeMs)
    {
        AIDebugLogger::Verbose("Cosmetics",
            "abandoning stalled random skin candidate %s for pawn=%p",
            Pending.CatalogEntry.Name.empty() ? "resident CID" : Pending.CatalogEntry.Name.c_str(),
            (void*)Pawn);
        PlayerAIResetPendingRandomSkinCandidate(Pending);
        Pending.RetryAt = Now + 16ULL;
        return EPlayerAIPendingRandomSkinResult::Keep;
    }

    if (Pending.RetryAt > Now)
        return EPlayerAIPendingRandomSkinResult::Keep;

    const bool bNeedsCandidate = !Pending.Character.Get() &&
        Pending.CatalogEntry.PrimaryAssetIdSize <= 0;
    if (bNeedsCandidate)
    {
        const bool bModernStartup = VersionInfo.FortniteVersion >= 19.0;
        const int MaxNovelCandidatesInFlight = bModernStartup ? 1 : 4;
        const int NovelCandidatesInFlight = PlayerAICountUnresolvedRandomSkinCandidates();
        if (bModernStartup && NovelCandidatesInFlight >= MaxNovelCandidatesInFlight)
        {
            Pending.RetryAt = Now + 100ULL;
            return EPlayerAIPendingRandomSkinResult::Keep;
        }
        const bool bPreferResolvedPool = !CachedResolvedSkinPool.empty() && (bModernStartup
                ? CachedResolvedSkinPool.size() >= 4 : (NovelCandidatesInFlight >=
                        MaxNovelCandidatesInFlight || (CachedResolvedSkinPool.size() >= 4 &&
                    rand() % 4 != 0)));
        if (bPreferResolvedPool)
        {
            if (auto PooledCharacter = PlayerAITryChooseCachedResolvedRandomSkin())
            {
                Pending.bUseResidentFallback = true;
                Pending.Character = TWeakObjectPtr<UObject>(PooledCharacter);
                Pending.CandidateAttempts++;
                if (!Pending.WorkStartedAt)
                {
                    Pending.WorkStartedAt = Now;
                    PlayerAIMarkPendingSkinCommitActive(Pawn);
                }
                Pending.CandidateStartedAt = Now;
                Pending.Phase = EPlayerAIPendingRandomSkinPhase::BaseParts;
            }
        }

        if (!Pending.Character.Get() && NovelCandidatesInFlight >= MaxNovelCandidatesInFlight)
        {
            Pending.RetryAt = Now + 100ULL;
            return EPlayerAIPendingRandomSkinResult::Keep;
        }
    }

    if (Pending.bUseResidentFallback && !Pending.Character.Get())
    {
        if (Pending.CandidateAttempts >= PlayerAIRandomSkinCandidatesPerBot)
        {
            return EPlayerAIPendingRandomSkinResult::Drop;
        }

        auto ResidentCharacter = PlayerAIChooseQueuedResidentSkin(Pending);
        if (!ResidentCharacter)
        {
            if (bLoadedSkinScanCompleted && CachedSkins.empty())
                return EPlayerAIPendingRandomSkinResult::Drop;

            Pending.RetryAt = Now + 50ULL;
            return EPlayerAIPendingRandomSkinResult::Keep;
        }

        Pending.CandidateAttempts++;
        if (!Pending.WorkStartedAt)
        {
            Pending.WorkStartedAt = Now;
            PlayerAIMarkPendingSkinCommitActive(Pawn);
        }
        Pending.CandidateStartedAt = Now;
        Pending.Character = TWeakObjectPtr<UObject>(ResidentCharacter);
        Pending.Phase = EPlayerAIPendingRandomSkinPhase::BaseParts;
    }
    else if (!Pending.bUseResidentFallback && Pending.CatalogEntry.PrimaryAssetIdSize <= 0)
    {
        if (Pending.CandidateAttempts >= PlayerAIRandomSkinCandidatesPerBot)
        {
            Pending.bUseResidentFallback = true;
            Pending.CandidateAttempts = 0;
            Pending.bResidentSelectionInitialized = false;
            PlayerAIResetPendingRandomSkinCandidate(Pending);
            Pending.RetryAt = Now + 50ULL;
            return EPlayerAIPendingRandomSkinResult::Keep;
        }

        if (!PlayerAIChooseQueuedCatalogSkin(Pending.CatalogEntry))
        {
            return EPlayerAIPendingRandomSkinResult::Drop;
        }

        Pending.CandidateAttempts++;
        if (!Pending.WorkStartedAt)
        {
            Pending.WorkStartedAt = Now;
            PlayerAIMarkPendingSkinCommitActive(Pawn);
        }
        Pending.CandidateStartedAt = Now;
        Pending.Phase = EPlayerAIPendingRandomSkinPhase::ResolveCharacter;
    }

    auto Character = Pending.bUseResidentFallback ? PlayerAIValidateCharacterDefinition(
            Pending.Character.Get()) : PlayerAIValidateCharacterDefinition(
            PlayerAITryResolveCatalogEntry(Pending.CatalogEntry,
                EPlayerAICosmeticLoadPolicy::ResidentOnly));
    const UObject* CachedParts[PlayerAICharacterPartSlotCount]{};
    auto PartClass = UCustomCharacterPart::StaticClass();
    if (Character && PartClass && PlayerAITryGetCachedResolvedSkinParts(
            Character, PartClass, CachedParts))
    {
        bool bTargetRetained = false;
        if (VersionFeatureAdapter::ApplyCharacterSkin(PlayerState, Pawn, Character,
                EPlayerAICosmeticLoadPolicy::ResidentOnly, &bTargetRetained))
        {
            PlayerAIMarkResolvedSkinVisuallyProven(Character);
            AFortPlayerPawnAthena::EnsurePlayerMapIcon(Controller, Pawn, Character);
            return EPlayerAIPendingRandomSkinResult::Applied;
        }

        if (bTargetRetained)
        {
            AFortPlayerPawnAthena::EnsurePlayerMapIcon(Controller, Pawn, Character);
            return EPlayerAIPendingRandomSkinResult::Applied;
        }

        if (PlayerAIIsCheatBotVisualCommitPending(Pawn, Character))
        {
            Pending.RetryAt = Now + 50ULL;
            return bPlayerAILastCheatBotVisualCallPerformedNativeWork
                ? EPlayerAIPendingRandomSkinResult::KeepAfterCosmeticAttempt
                : EPlayerAIPendingRandomSkinResult::Keep;
        }

        return EPlayerAIPendingRandomSkinResult::DropAfterCosmeticAttempt;
    }

    if (!Pending.bUseResidentFallback && Pending.Phase ==
        EPlayerAIPendingRandomSkinPhase::ResolveCharacter)
    {
        auto CharacterResult = PlayerAIResolveOrRequestQueuedCatalogSkin(
                Pending.CatalogEntry, Pawn);
        if (bPrimaryAssetCosmeticFunctionsDisabled)
        {
            Pending.bUseResidentFallback = true;
            PlayerAIResetPendingRandomSkinCandidate(Pending);
            Pending.RetryAt = Now + 50ULL;
            return EPlayerAIPendingRandomSkinResult::Keep;
        }

        if (CharacterResult.State == PlayerLoadout::EPreviewTextureLoadState::Pending)
        {
            Pending.RetryAt = Now + (std::max)(static_cast<ULONGLONG>(50), static_cast<ULONGLONG>(
                    CharacterResult.RetryAfterMs));
            return EPlayerAIPendingRandomSkinResult::Keep;
        }

        Character = PlayerAIValidateCharacterDefinition(CharacterResult.Object);
        if (!Character)
        {
            PlayerAIResetPendingRandomSkinCandidate(Pending);
            Pending.RetryAt = Now + 50ULL;
            return EPlayerAIPendingRandomSkinResult::Keep;
        }

        Pending.Character = TWeakObjectPtr<UObject>(Character);
        Pending.CandidateStartedAt = Now;
        Pending.Phase = EPlayerAIPendingRandomSkinPhase::BaseParts;
    }

    Character = PlayerAIValidateCharacterDefinition(Pending.Character.Get());
    if (!Character)
    {
        PlayerAIResetPendingRandomSkinCandidate(Pending);
        Pending.RetryAt = Now + 50ULL;
        return EPlayerAIPendingRandomSkinResult::Keep;
    }

    const auto AdvanceResult = PlayerAIAdvanceQueuedCharacterParts(Pending, Character, Now);
    if (AdvanceResult == EPlayerAIQueuedPartAdvanceResult::Keep)
        return EPlayerAIPendingRandomSkinResult::Keep;

    if (AdvanceResult == EPlayerAIQueuedPartAdvanceResult::Ready &&
        PlayerAICacheCompletedQueuedSkinParts(Pending, Character))
    {
        bool bTargetRetained = false;
        if (VersionFeatureAdapter::ApplyCharacterSkin(PlayerState, Pawn, Character,
                EPlayerAICosmeticLoadPolicy::ResidentOnly, &bTargetRetained))
        {
            PlayerAIMarkResolvedSkinVisuallyProven(Character);
            AFortPlayerPawnAthena::EnsurePlayerMapIcon(Controller, Pawn, Character);
            AIDebugLogger::Verbose("Cosmetics",
                "applied deferred random skin %s to pawn=%p after %d candidates",
                Character->Name.ToString().c_str(), (void*)Pawn, Pending.CandidateAttempts);
            return EPlayerAIPendingRandomSkinResult::Applied;
        }

        if (bTargetRetained)
        {
            AFortPlayerPawnAthena::EnsurePlayerMapIcon(Controller, Pawn, Character);
            return EPlayerAIPendingRandomSkinResult::Applied;
        }

        if (PlayerAIIsCheatBotVisualCommitPending(Pawn, Character))
        {
            Pending.RetryAt = Now + 50ULL;
            return bPlayerAILastCheatBotVisualCallPerformedNativeWork
                ? EPlayerAIPendingRandomSkinResult::KeepAfterCosmeticAttempt
                : EPlayerAIPendingRandomSkinResult::Keep;
        }

        return EPlayerAIPendingRandomSkinResult::DropAfterCosmeticAttempt;
    }

    PlayerAIResetPendingRandomSkinCandidate(Pending);
    Pending.RetryAt = Now + 50ULL;
    return EPlayerAIPendingRandomSkinResult::Keep;
}

static void PlayerAITickPendingRandomBotSkins()
{
    if (PendingRandomBotSkins.empty())
        return;

    const ULONGLONG Now = GetTickCount64();
    int Inspected = 0;
    while (!PendingRandomBotSkins.empty() && Inspected < PlayerAIRandomSkinRecordsPerTick)
    {
        if (PendingRandomBotSkinCursor >= PendingRandomBotSkins.size())
        {
            PendingRandomBotSkinCursor = 0;
        }

        const size_t Index = PendingRandomBotSkinCursor;
        const auto Result = PlayerAITickPendingRandomBotSkin(PendingRandomBotSkins[Index], Now);
        Inspected++;

        if (Result == EPlayerAIPendingRandomSkinResult::KeepAfterCosmeticAttempt)
        {
            PendingRandomBotSkinCursor++;
            break;
        }

        if (Result != EPlayerAIPendingRandomSkinResult::Keep)
        {
            PlayerAIReleasePendingSkinCommit(PendingRandomBotSkins[Index].PawnIdentity);
            PendingRandomBotSkins.erase(PendingRandomBotSkins.begin() + Index);
            if (Result == EPlayerAIPendingRandomSkinResult::Applied ||
                Result == EPlayerAIPendingRandomSkinResult::DropAfterCosmeticAttempt)
            {
                break;
            }
            continue;
        }

        PendingRandomBotSkinCursor++;
    }

    if (PendingRandomBotSkinCursor >= PendingRandomBotSkins.size())
    {
        PendingRandomBotSkinCursor = 0;
    }
}

bool VersionFeatureAdapter::QueueRequestedSkin(AFortPlayerControllerAthena* PC,
    AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn,
    UAthenaCharacterItemDefinition* Character)
{
    auto World = UWorld::GetWorld();
    if (!World || !PC || !PlayerState || !Pawn || !PlayerAIIsLiveSupportObject(PC) ||
        !PlayerAIIsLiveSupportObject(PlayerState) || !PlayerAIIsLiveSupportObject(Pawn) ||
        PC->PlayerState != PlayerState || Pawn->PlayerState != PlayerState ||
        (PC->Pawn != Pawn && PC->MyFortPawn != Pawn) || (Character &&
         !PlayerAIValidateCharacterDefinition(Character)))
    {
        return false;
    }

    for (const auto& Pending : PendingRequestedBotSkinCommits)
    {
        if (Pending.Pawn.Get() == Pawn)
            return true;
    }
    for (const auto& Pending : PendingRandomBotSkins)
    {
        if (Pending.Pawn.Get() == Pawn)
            return true;
    }

    if (PendingRequestedBotSkinCommits.size() + PendingRandomBotSkins.size() >=
        PlayerAIMaxPendingRandomBotSkins)
    {
        AIDebugLogger::MissingFeature("DeferredBotSkinCommitCapacity",
            "the cosmetic queue is full; this bot keeps its engine appearance");
        return false;
    }

    FPlayerAIPendingRequestedSkinCommit Pending{};
    Pending.World = TWeakObjectPtr<UWorld>(World);
    Pending.Controller = TWeakObjectPtr<AFortPlayerControllerAthena>(PC);
    Pending.PlayerState = TWeakObjectPtr<AFortPlayerStateAthena>(PlayerState);
    Pending.Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
    Pending.PawnIdentity = Pawn;
    Pending.Character = TWeakObjectPtr<UObject>(Character);
    Pending.QueuedAt = GetTickCount64();
    Pending.bUseDefault = Character == nullptr;
    PendingRequestedBotSkinCommits.push_back(std::move(Pending));
    PlayerAITrackPendingSkinCommit(Pawn);
    return true;
}

bool VersionFeatureAdapter::QueueRandomSkin(AFortPlayerControllerAthena* PC,
    AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn,
    UAthenaCharacterItemDefinition** AppliedImmediately)
{
    if (AppliedImmediately)
        *AppliedImmediately = nullptr;

    auto World = UWorld::GetWorld();
    if (!World || !PC || !PlayerState || !Pawn || !PlayerAIIsLiveSupportObject(PC) ||
        !PlayerAIIsLiveSupportObject(PlayerState) || !PlayerAIIsLiveSupportObject(Pawn) ||
        PC->PlayerState != PlayerState || Pawn->PlayerState != PlayerState ||
        (PC->Pawn != Pawn && PC->MyFortPawn != Pawn))
    {
        return false;
    }

    // 1.x predates primary-asset enumeration.
    if (VersionInfo.FortniteVersion < 2.00)
    {
        auto Character = PlayerAITryChooseCachedResolvedRandomSkin();
        if (!Character && !CachedSkins.empty())
        {
            const size_t Start = static_cast<size_t>(rand()) % CachedSkins.size();
            const size_t Attempts = (std::min)(CachedSkins.size(), size_t(8));
            for (size_t Offset = 0;
                 Offset < Attempts; Offset++)
            {
                Character = PlayerAIValidateCharacterDefinition(CachedSkins[(Start + Offset) %
                        CachedSkins.size()]);
                if (Character)
                    break;
            }
        }
        return QueueRequestedSkin(PC, PlayerState, Pawn, Character);
    }

    for (const auto& Pending : PendingRandomBotSkins)
    {
        if (Pending.Pawn.Get() == Pawn)
            return true;
    }
    for (const auto& Pending : PendingRequestedBotSkinCommits)
    {
        if (Pending.Pawn.Get() == Pawn)
            return true;
    }

    if (PendingRandomBotSkins.size() + PendingRequestedBotSkinCommits.size() >=
        PlayerAIMaxPendingRandomBotSkins)
    {
        AIDebugLogger::MissingFeature("DeferredRandomSkinQueueCapacity",
            "the cosmetic queue is full; additional bots keep their safe default outfit");
        return false;
    }

    FPlayerAIPendingRandomSkin Pending{};
    Pending.World = TWeakObjectPtr<UWorld>(World);
    Pending.Controller = TWeakObjectPtr<AFortPlayerControllerAthena>(PC);
    Pending.PlayerState = TWeakObjectPtr<AFortPlayerStateAthena>(PlayerState);
    Pending.Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
    Pending.PawnIdentity = Pawn;
    Pending.QueuedAt = GetTickCount64();
    Pending.bUseResidentFallback = bPrimaryAssetCosmeticFunctionsDisabled;
    PendingRandomBotSkins.push_back(std::move(Pending));
    PlayerAITrackPendingSkinCommit(Pawn);
    return true;
}

void VersionFeatureAdapter::ResetCaches()
{
    for (auto& Pair : PendingCheatBotVisualCommits)
        PlayerAIRestorePendingCheatBotVisualCommit(Pair.second);

    PlayerAILegacyAircraftExitLatchCache = {};
    bEmoteCacheBuilt = false;
    CachedEmoteAssets.clear();
    CachedSkins.clear();
    CachedSkinObjects.clear();
    CachedSkinsByLowerName.clear();
    CachedResolvedSkinParts.clear();
    CachedResolvedSkinPool.clear();
    CachedSkinCatalog.clear();
    CachedSkinCatalogByLowerName.clear();
    PendingRandomBotSkins.clear();
    PendingRequestedBotSkinCommits.clear();
    PendingBotSkinCommitPawnCounts.clear();
    PendingBotSkinActivePawns.clear();
    PendingCheatBotVisualCommits.clear();
    for (auto Settle = PendingBotSkinSettles.begin();
         Settle != PendingBotSkinSettles.end();)
    {
        if (Settle->second.World.Get() != UWorld::GetWorld() ||
            Settle->second.Pawn.Get() != Settle->first)
        {
            Settle = PendingBotSkinSettles.erase(Settle);
        }
        else
        {
            ++Settle;
        }
    }
    RandomSkinCatalogShuffleStart = 0;
    RandomSkinCatalogShuffleCursor = 0;
    RandomSkinCatalogShuffleSize = 0;
    LastQueuedRandomSkinName.clear();
    PendingRandomBotSkinCursor = 0;
    PlayerAINoteBotSkinProgress();
    PlayerAIClearPendingSkinCatalog();
    SkinCacheAttempts = 0;
    SkinCatalogAttempts = 0;
    SkinCatalogRetryTicks = 0;
    LastSkinCatalogAttemptServerTime = -1.f;
    bKnownSkinsLoaded = false;
    bLoadedSkinScanCompleted = false;
    bSkinCatalogReady = false;
    KnownSkinScanCursor = 0;
    LoadedSkinScanCursor = 0;
    LoadedSkinScanLimit = 0;
    bCosmeticPathLoadDisabled = false;
    bCosmeticPackageLoadDisabled = false;
    bSoftCosmeticResolveDisabled = false;
    bSoftCosmeticLoadDisabled = false;
    bPrimaryAssetCosmeticFunctionsDisabled = false;
    bDefaultCommandoResolved = false;
    PlayerAIDefaultCommando = nullptr;
    bDefaultPartsResolved = false;
    PlayerAIDefaultHead = nullptr;
    PlayerAIDefaultBody = nullptr;
    PlayerAIDefaultBackpack = nullptr;
    for (int Index = 0;
         Index < PlayerAICharacterPartSlotCount; Index++)
    {
        PlayerAIResidentPlaceholderParts[Index] = nullptr;
        PlayerAIResidentPlaceholderPartIndices[Index] = -1;
    }
    PlayerAIResidentPlaceholderHero = nullptr;
    PlayerAIResidentPlaceholderHeroIndex = -1;
    PlayerAIResidentPlaceholderDonorCursor = 0;
    PlayerAIResidentPlaceholderRetryAt = 0;
    bPlayerAIServerChoosePartDisabled = false;
    bPlayerAIServerSetCosmeticLoadoutDisabled = false;
    bPlayerAIApplyCharacterCosmeticsDisabled = false;
    bPlayerAIRequestedDefaultSynchronousAttempted = false;
    PlayerAIPhaseLogicWorld = nullptr;
    PlayerAIPhaseLogic = nullptr;
    PlayerAIPhaseLogicClass = nullptr;
    PlayerAINextPhaseLogicClassResolveTime = 0;
    PlayerAINextPhaseLogicResolveTime = 0;
    PlayerAIPhaseLogicScanCursor = 0;
    PlayerAIPhaseLogicScanLimit = 0;
    PlayerAIResetClassLookup(PlayerAIAircraftComponentClassCache);
    PlayerAIResetClassLookup(PlayerAIPawnClassCache);
    PlayerAIResetClassLookup(PlayerAIControllerClassCache);
    PlayerAIResetClassLookup(PlayerAIMovementComponentClassCache);
    PlayerAIResetClassLookup(PlayerAISafeZoneIndicatorClassCache);
    PlayerAIResetClassLookup(PlayerAINetPushModelHelpersClassCache);
    PlayerAIResetClassLookup(PlayerAISkeletalMeshClassCache);
    PlayerAIFortAthenaLoadoutStructCache = nullptr;
    PlayerAIPushModelHelpers = nullptr;
    PlayerAIMarkPropertyDirtyFunction = nullptr;
    PlayerAIMarkPropertyDirtyFunctionIndex = -1;
    PlayerAINextPushModelHelpersResolveTime = 0;
    PlayerAIPlayersLeftDirtyPending = false;
    bGroundTraceDisabled = false;
    GroundTraceCalls = 0;
    GroundTraceHits = 0;
    PlayerAILandingProbeCursor = 0;
    PlayerAILastServerTickTime = -1.f;
    PlayerAILastBudgetWallTickMs = 0;
    PlayerAIGroundTraceBudgetRemaining = PlayerAIGroundTraceBudgetPerTick;
    PlayerAIPhaseScanBudgetAvailable = true;
    PlayerAIVisualProofBudgetAvailable = true;
}
