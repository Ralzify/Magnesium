#include "pch.h"
// ============================================================================
// Magnesium PlayerAI - support modules
//   AIDebugLogger, AINameGenerator, AISkillProfile, VersionFeatureAdapter
// ============================================================================
#include "../Public/AIDebugLogger.h"
#include "../Public/AINameGenerator.h"
#include "../Public/AISkillProfile.h"
#include "../Public/PlayerAIConfig.h"
#include "../Public/PlayerAIFaultGuard.h"
#include "../Public/PlayerAIManager.h"
#include "../Public/VersionFeatureAdapter.h"
#include "../../Public/Configuration.h"
#include "../../Public/Finders.h"
#include "../../Public/GUI.h"
#include "../../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../../FortniteGame/Public/FortKismetLibrary.h"
#include "../../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../../FortniteGame/Public/FortWeapon.h"
#include <cstdarg>
#include <unordered_set>
#include <vector>
#include <string>

// ============================================================================
// AIDebugLogger
// ============================================================================

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
    printf("[PlayerAI][MissingFeature] '%s' is not supported on this version - %s\n", FeatureName, FallbackDescription);
}

void AIDebugLogger::Error(const char* Category, const char* Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    PlayerAILogInternal("[Error]", Category, Format, Args);
    va_end(Args);
}

// ============================================================================
// AINameGenerator
// ============================================================================

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

        // Roughly half the names get a short number suffix, like real players.
        if (rand() % 2)
            Name += std::to_string(rand() % 100);

        if (!UsedPlayerAINames.count(Name))
        {
            UsedPlayerAINames.insert(Name);
            return Name;
        }
    }

    // Deterministic fallback: never publish a collision even after a very
    // full lobby or an unlucky random sequence.
    for (;;)
    {
        std::string Name =
            "AIPlayer" + std::to_string(NextFallbackPlayerAIName++);

        if (UsedPlayerAINames.insert(Name).second)
            return Name;
    }
}

void AINameGenerator::Reset()
{
    UsedPlayerAINames.clear();
    NextFallbackPlayerAIName = 1;
}

// ============================================================================
// AISkillProfile
// ============================================================================

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
        // Internal testing profile only - intentionally unfair aim.
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
    // Weighted distribution; Testing is never picked randomly.
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

// ============================================================================
// VersionFeatureAdapter
// ============================================================================

// External Magnesium symbol (defined in FortGameMode.cpp) used to apply
// character customization to newly possessed pawns - same path real player
// pawns and the existing systems use.
extern uint64_t ApplyCharacterCustomization;

// ---- Guarded native invocation --------------------------------------------

// (Kept free of unwindable C++ objects so SEH is allowed here. The depth
// counter tells the vectored crash reporter to defer to this handler.)
static bool PlayerAIGuardedProcessEvent(const UObject* Obj, UFunction* Fn, void* Params)
{
    GPlayerAIGuardedNativeCallDepth++;
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

    GPlayerAIGuardedNativeCallDepth--;
    return bOk;
}

bool VersionFeatureAdapter::SafeCallNoArgs(const UObject* Obj, UFunction* Fn)
{
    if (!Obj || !Fn)
        return false;

    // Zeroed, correctly sized parameter buffer: never pass null parms to a
    // native that expects arguments (a classic per-version pitfall).
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
static constexpr int PlayerAIGroundTraceBudgetPerTick = 4;
static int PlayerAIGroundTraceBudgetRemaining =
    PlayerAIGroundTraceBudgetPerTick;
static bool PlayerAIPhaseScanBudgetAvailable = true;

void VersionFeatureAdapter::BeginServerTick(float TimeSeconds)
{
    // TickFlush can reach the integration through more than one version
    // hook. Do not reopen the budget while world time is unchanged.
    if (TimeSeconds == PlayerAILastServerTickTime)
        return;

    PlayerAILastServerTickTime = TimeSeconds;
    PlayerAIGroundTraceBudgetRemaining =
        PlayerAIGroundTraceBudgetPerTick;
    PlayerAIPhaseScanBudgetAvailable = true;
}

static bool PlayerAIIsLiveSupportObject(const UObject* Object)
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

static bool PlayerAIIsLiveSupportActor(const AActor* Actor)
{
    if (!PlayerAIIsLiveSupportObject(Actor))
        return false;

    return !Actor->HasbActorIsBeingDestroyed() ||
        !Actor->bActorIsBeingDestroyed;
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

static const UClass* PlayerAIResolveCachedClass(
    FPlayerAIClassLookupCache& Cache, const char* ClassName)
{
    if (Cache.Class &&
        PlayerAIIsLiveSupportObject(Cache.Class))
    {
        return Cache.Class;
    }

    if (Cache.Class)
    {
        // The object slot no longer owns this class pointer. Permit one
        // immediate re-resolution rather than retaining a stale generation.
        Cache.Class = nullptr;
        Cache.NextResolveTime = 0;
    }

    const ULONGLONG Now = GetTickCount64();

    if (Now < Cache.NextResolveTime)
        return nullptr;

    // FindClass scans the complete object array. Successful lookups remain
    // cached; unavailable/late-loaded classes retry at a low fixed cadence.
    Cache.Class = FindClass(ClassName);
    Cache.NextResolveTime =
        Cache.Class ? 0 : Now + 2000ULL;
    return Cache.Class;
}

static void PlayerAIResetClassLookup(
    FPlayerAIClassLookupCache& Cache)
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

static bool PlayerAIIsOwnedByGameState(
    const UObject* Object, const AFortGameStateAthena* GameState)
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

// FN 25.20+ moved the authoritative BR phase, aircraft, and safe-zone state
// from FortGameState/FortGameMode onto this component. Resolve by ownership
// rather than using a process-static "first object" so map travel cannot
// return a component from the previous world.
static UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
PlayerAIResolvePhaseLogic()
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

    if (Now < PlayerAINextPhaseLogicResolveTime ||
        !PlayerAIPhaseScanBudgetAvailable)
        return nullptr;

    // GetMatchPhase, GetAircraft, and TryGetSafeZone can all resolve this
    // component during one server tick. Only one bounded slice may scan.
    PlayerAIPhaseScanBudgetAvailable = false;

    if (PlayerAIPhaseLogicClass &&
        !PlayerAIIsLiveSupportObject(PlayerAIPhaseLogicClass))
    {
        // A class pointer can become stale after world/package teardown.
        // Restart both lookups instead of carrying a scan across generations.
        PlayerAIPhaseLogicClass = nullptr;
        PlayerAINextPhaseLogicClassResolveTime = 0;
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = 0;
    }

    if (!PlayerAIPhaseLogicClass)
    {
        if (Now < PlayerAINextPhaseLogicClassResolveTime)
            return nullptr;

        // FindClass is itself a full object-array scan. Memoize the result so
        // each 512-object component slice does not first rescan the array.
        PlayerAIPhaseLogicClass =
            FindClass(
                "FortGameStateComponent_BattleRoyaleGamePhaseLogic");

        if (!PlayerAIPhaseLogicClass)
        {
            // Retain late-load compatibility without retrying every tick.
            PlayerAINextPhaseLogicClassResolveTime =
                Now + 2000ULL;
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
    const int End = (std::min)(
        PlayerAIPhaseLogicScanCursor + ScanBudget,
        PlayerAIPhaseLogicScanLimit);

    for (int i = PlayerAIPhaseLogicScanCursor;
         i < End && i < TUObjectArray::Num(); i++)
    {
        auto Object = TUObjectArray::GetObjectByIndex(i);

        if (!PlayerAIIsLiveSupportObject(Object) ||
            Object->IsDefaultObject() ||
            !Object->IsA(ComponentClass) ||
            !PlayerAIIsOwnedByGameState(Object, GameState))
        {
            continue;
        }

        PlayerAIPhaseLogic =
            (UFortGameStateComponent_BattleRoyaleGamePhaseLogic*)Object;
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = 0;
        return PlayerAIPhaseLogic;
    }

    PlayerAIPhaseLogicScanCursor = End;

    if (PlayerAIPhaseLogicScanCursor >=
        PlayerAIPhaseLogicScanLimit)
    {
        PlayerAIPhaseLogicScanCursor = 0;
        PlayerAIPhaseLogicScanLimit = 0;
        PlayerAINextPhaseLogicResolveTime = Now + 2000ULL;
    }

    return nullptr;
}

static UObject* PlayerAIPushModelHelpers = nullptr;
static UFunction* PlayerAIMarkPropertyDirtyFunction = nullptr;
static ULONGLONG PlayerAINextPushModelHelpersResolveTime = 0;
static bool PlayerAIPlayersLeftDirtyPending = false;

static bool PlayerAIValidateMarkPropertyDirty(UFunction* Function)
{
    if (!PlayerAIIsLiveSupportObject(Function))
        return false;

    auto Params = Function->GetParamsNamed();

    if (VersionInfo.FortniteVersion < 32.0 &&
        Params.Size != 0x10)
        return false;

    bool bHasObject = false;
    bool bHasPropertyName = false;

    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "Object")
        {
            bHasObject = Param.Offset == 0 &&
                (VersionInfo.FortniteVersion >= 32.0 ||
                 (Param.ElementSize == sizeof(UObject*) &&
                  (Param.PropertyFlags & 0x80) != 0));
        }
        else if (Param.Name == "PropertyName")
        {
            bHasPropertyName = Param.Offset == 0x8 &&
                (VersionInfo.FortniteVersion >= 32.0 ||
                 ((Param.ElementSize == sizeof(int32) ||
                   Param.ElementSize == sizeof(FName)) &&
                  (Param.PropertyFlags & 0x80) != 0));
        }
    }

    return bHasObject && bHasPropertyName;
}

bool VersionFeatureAdapter::MarkReplicatedPropertyDirty(
    const UObject* Object, const wchar_t* PropertyName)
{
    if (VersionInfo.FortniteVersion < 19.0 ||
        !PlayerAIIsLiveSupportObject(Object) || !PropertyName)
    {
        return false;
    }

    if (!PlayerAIIsLiveSupportObject(PlayerAIPushModelHelpers) ||
        !PlayerAIValidateMarkPropertyDirty(
            PlayerAIMarkPropertyDirtyFunction))
    {
        PlayerAIPushModelHelpers = nullptr;
        PlayerAIMarkPropertyDirtyFunction = nullptr;

        const ULONGLONG Now = GetTickCount64();

        if (Now < PlayerAINextPushModelHelpersResolveTime)
            return false;

        // GetDefaultObj and the FindClass fallback can both scan globally
        // when their SDK offsets/classes are unavailable. Resolve at most
        // once per backoff interval until the complete helper is usable.
        PlayerAINextPushModelHelpersResolveTime =
            Now + 2000ULL;

        auto HelpersClass =
            Offsets::StaticFindObject
            ? (UClass*)SDK::StaticFindObject(
                L"/Script/Engine.NetPushModelHelpers",
                UClass::StaticClass())
            : nullptr;

        if (!HelpersClass)
            HelpersClass =
                const_cast<UClass*>(
                    PlayerAIResolveCachedClass(
                        PlayerAINetPushModelHelpersClassCache,
                        "NetPushModelHelpers"));

        PlayerAIPushModelHelpers =
            HelpersClass ? HelpersClass->GetDefaultObj() : nullptr;
        PlayerAIMarkPropertyDirtyFunction =
            PlayerAIPushModelHelpers
            ? PlayerAIPushModelHelpers->GetFunction("MarkPropertyDirty")
            : nullptr;

        if (!PlayerAIIsLiveSupportObject(PlayerAIPushModelHelpers) ||
            !PlayerAIValidateMarkPropertyDirty(
                PlayerAIMarkPropertyDirtyFunction))
        {
            PlayerAIPushModelHelpers = nullptr;
            PlayerAIMarkPropertyDirtyFunction = nullptr;
            return false;
        }

        PlayerAINextPushModelHelpersResolveTime = 0;
    }

    bool bSucceeded = false;
    GPlayerAIGuardedNativeCallDepth++;

    __try
    {
        FName ReplicatedPropertyName(PropertyName);
        PlayerAIPushModelHelpers->Call<void>(
            PlayerAIMarkPropertyDirtyFunction,
            (UObject*)Object,
            ReplicatedPropertyName);
        bSucceeded = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        PlayerAIPushModelHelpers = nullptr;
        PlayerAIMarkPropertyDirtyFunction = nullptr;
        PlayerAINextPushModelHelpersResolveTime =
            GetTickCount64() + 2000ULL;
    }

    GPlayerAIGuardedNativeCallDepth--;
    return bSucceeded;
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

    // Some builds publish their engine roster late (or remove a dead bot
    // late). The manager supplies live AIs and explicitly subtracts managed
    // dead entries, while the set keeps all normal engine participants.
    for (auto& Managed : PlayerAIManager::GetControllers())
    {
        if (!Managed)
            continue;

        const AActor* ManagedController =
            Managed->Entity.bNativeBacked
            ? Managed->Entity.NativeController
            : (const AActor*)Managed->Entity.PC;

        if (!ManagedController)
            continue;

        if (Managed->IsAlive())
            UniqueControllers.insert(ManagedController);
        else
            UniqueControllers.erase(ManagedController);
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

    if (!bChanged && !bForce &&
        !PlayerAIPlayersLeftDirtyPending)
        return;

    GameState->PlayersLeft = PlayersLeft;

    if (VersionInfo.FortniteVersion >= 19.0)
    {
        PlayerAIPlayersLeftDirtyPending =
            !MarkReplicatedPropertyDirty(
                GameState, L"PlayersLeft");
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
        ReplicatePlayersLeft(
            GameState, CountAliveParticipants(), bForce);
}

void VersionFeatureAdapter::RetryPendingPlayersLeftReplication()
{
    if (!PlayerAIPlayersLeftDirtyPending)
        return;

    auto GameState = GetGameState();

    if (GameState)
        ReplicatePlayersLeft(
            GameState, GameState->PlayersLeft, true);
}

int VersionFeatureAdapter::GetMaxPlayerCount()
{
    auto GameMode = GetGameMode();

    if (GameMode && GameMode->HasGameSession() && GameMode->GameSession && GameMode->GameSession->HasMaxPlayers())
    {
        int Max = GameMode->GameSession->MaxPlayers;

        if (Max > 0 && Max <= 255)
            return Max;
    }

    // Fallback: playlist max players.
    auto GameState = GetGameState();
    const UFortPlaylistAthena* Playlist = nullptr;

    if (GameState)
    {
        if (GameState->HasCurrentPlaylistInfo())
            Playlist = GameState->CurrentPlaylistInfo.BasePlaylist;
        else if (GameState->HasCurrentPlaylistData())
            Playlist = GameState->CurrentPlaylistData;
    }

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

    // Component-driven BR seasons (including 27.11 and 30.00) can retain a
    // legacy GameState phase field that is no longer authoritative.
    if (auto PhaseLogic = PlayerAIResolvePhaseLogic())
    {
        const auto GamePhaseOffset = PhaseLogic->GetOffset("GamePhase");

        if (GamePhaseOffset != (uint32)-1 &&
            GamePhaseOffset < 0x10000)
        {
            const uint8 Phase =
                GetFromOffset<uint8>(PhaseLogic, GamePhaseOffset);

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

    // GamePhase (when present): 2 = Warmup, 3 = Aircraft, 4 = SafeZones, 5 = EndGame.
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

// ---- Emotes -----------------------------------------------------------------

static bool bEmoteCacheBuilt = false;
static std::vector<UObject*> CachedEmoteAssets;

static void BuildEmoteCache()
{
    if (bEmoteCacheBuilt)
        return;

    bEmoteCacheBuilt = true;

    // Broadly available emote assets across versions; missing ones are
    // simply filtered out. If none exist, emotes are unsupported.
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
        auto Asset =
            Offsets::StaticFindObject
            ? (const UAthenaDanceItemDefinition*)
                SDK::StaticFindObject(
                    Path,
                    UAthenaDanceItemDefinition::
                    StaticClass())
            : nullptr;

        if (Asset)
            CachedEmoteAssets.push_back((UObject*)Asset);
    }

    if (CachedEmoteAssets.empty())
        AIDebugLogger::MissingFeature("Emotes", "PlayerAI players keep walking/idling/jumping instead");
    else
        AIDebugLogger::Log("Emotes", "%d emote assets available on this version", (int)CachedEmoteAssets.size());
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

// ---- Transport / bus ----------------------------------------------------------

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

    // Sized zeroed parameters + fault guard: safe regardless of the exact
    // signature on this version.
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
    auto ValidateAircraft =
        [](AFortAthenaAircraft* Aircraft)
        -> AFortAthenaAircraft*
        {
            auto AircraftClass =
                AFortAthenaAircraft::StaticClass();

            return AircraftClass &&
                PlayerAIIsLiveSupportActor(Aircraft) &&
                Aircraft->IsA(AircraftClass)
                ? Aircraft
                : nullptr;
        };

    if (auto PhaseLogic = PlayerAIResolvePhaseLogic())
    {
        if (PhaseLogic->HasAircrafts_GameState() &&
            PhaseLogic->Aircrafts_GameState.Num() > 0)
        {
            if (auto Aircraft =
                ValidateAircraft(
                    PhaseLogic->Aircrafts_GameState[0].Get()))
            {
                return Aircraft;
            }
        }

        if (PhaseLogic->HasAircrafts_GameMode() &&
            PhaseLogic->Aircrafts_GameMode.Num() > 0)
        {
            if (auto Aircraft =
                ValidateAircraft(
                    PhaseLogic->Aircrafts_GameMode[0].Get()))
            {
                return Aircraft;
            }
        }
    }

    auto GameState = GetGameState();

    if (!GameState)
        return nullptr;

    if (GameState->HasAircrafts())
        return GameState->Aircrafts.Num() > 0
            ? ValidateAircraft(GameState->Aircrafts[0])
            : nullptr;

    if (GameState->HasAircraft())
        return ValidateAircraft(GameState->Aircraft);

    return nullptr;
}

EPlayerAIAircraftDropState
VersionFeatureAdapter::GetAircraftDropState(float TimeSeconds)
{
    bool bLocked = false;
    bool bOpen = false;

    auto ReadPhaseStep =
        [&bLocked, &bOpen](const UObject* Owner)
        {
            if (!Owner)
                return;

            const auto Offset = Owner->GetOffset(
                "GamePhaseStep");

            if (Offset == (uint32)-1 ||
                Offset >= 0x10000 ||
                !SDK::MemReadable(
                    (const uint8*)Owner + Offset,
                    sizeof(uint8)))
            {
                return;
            }

            const uint8 Step =
                GetFromOffset<uint8>(Owner, Offset);

            if (Step == (uint8)EAthenaGamePhaseStep::BusLocked)
                bLocked = true;
            else if (Step >=
                (uint8)EAthenaGamePhaseStep::BusFlying)
                bOpen = true;
        };

    // Newer builds move the authoritative phase step onto this component.
    // A false bAircraftIsLocked is not sufficient by itself: some versions
    // publish false while their phase step still says BusLocked.
    auto PhaseLogic = PlayerAIResolvePhaseLogic();

    if (PhaseLogic)
    {
        ReadPhaseStep(PhaseLogic);

        if (PhaseLogic->HasbAircraftIsLocked() &&
            PhaseLogic->bAircraftIsLocked)
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

        if (Aircraft->HasDropStartTime() &&
            std::isfinite((double)Aircraft->DropStartTime) &&
            Aircraft->DropStartTime > 1.f)
        {
            DropStart = Aircraft->DropStartTime;
            bHaveStartSignal = true;
        }
        else if (Aircraft->HasFlightStartTime() &&
            Aircraft->HasTimeTillDropStart() &&
            std::isfinite((double)Aircraft->FlightStartTime) &&
            std::isfinite((double)Aircraft->TimeTillDropStart) &&
            Aircraft->FlightStartTime > 0.f &&
            Aircraft->TimeTillDropStart >= 0.f)
        {
            DropStart =
                Aircraft->FlightStartTime +
                Aircraft->TimeTillDropStart;
            bHaveStartSignal = true;
        }
        else if (Aircraft->HasFlightElapsedTime() &&
            Aircraft->HasTimeTillDropStart() &&
            std::isfinite((double)Aircraft->FlightElapsedTime) &&
            std::isfinite((double)Aircraft->TimeTillDropStart) &&
            Aircraft->TimeTillDropStart >= 0.f)
        {
            // Relative timing is enough when an absolute timestamp is not
            // exposed by the hosted build.
            bHaveStartSignal = true;

            if (Aircraft->FlightElapsedTime + 0.01f <
                Aircraft->TimeTillDropStart)
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

    // Fail closed on contradictory metadata. The caller can force-open only
    // after the authoritative match phase advances or the drop-zone-ending
    // callback queues the remaining passengers.
    if (bLocked)
        return EPlayerAIAircraftDropState::Locked;
    if (bOpen)
        return EPlayerAIAircraftDropState::Open;
    return EPlayerAIAircraftDropState::Unknown;
}

static bool PlayerAITryReadInAircraftBit(
    const UObject* Object, bool& OutInAircraft)
{
    if (!Object)
        return false;

    auto Property =
        Object->GetProperty("bInAircraft", 0x20000);

    if (!Property)
        return false;

    const auto Offset = GetFromOffset<uint32>(
        Property, Offsets::Offset_Internal);
    const auto Mask = Property->GetFieldMask();

    if (!Mask || Offset >= 0x20000)
        return false;

    OutInAircraft =
        (GetFromOffset<uint8>(Object, Offset) & Mask) != 0;
    return true;
}

static bool PlayerAITryQueryInAircraft(
    AFortPlayerControllerAthena* PC,
    UFunction* Function, bool& OutInAircraft)
{
    if (!PC || !Function)
        return false;

    bool bCalled = false;
    GPlayerAIGuardedNativeCallDepth++;

    __try
    {
        OutInAircraft =
            PC->Call<bool>(Function);
        bCalled = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    GPlayerAIGuardedNativeCallDepth--;
    return bCalled;
}

bool VersionFeatureAdapter::IsInAircraft(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return false;

    // Aircraft state moved between the controller, PlayerState and a
    // controller component over Fortnite's lifetime. A readable false value
    // on one mirror is not authoritative: aggregate every available source
    // and report aboard if any native mirror is still set.
    if (auto Function = PC->GetFunction("IsInAircraft"))
    {
        bool bInAircraft = false;

        if (PlayerAITryQueryInAircraft(
                PC, Function, bInAircraft) &&
            bInAircraft)
        {
            return true;
        }
    }

    bool bInAircraft = false;

    if (PlayerAITryReadInAircraftBit(
            PC, bInAircraft) &&
        bInAircraft)
    {
        return true;
    }

    if (PC->PlayerState &&
        PlayerAITryReadInAircraftBit(
            PC->PlayerState, bInAircraft) &&
        bInAircraft)
    {
        return true;
    }

    auto AircraftComponentClass =
        PlayerAIResolveCachedClass(
            PlayerAIAircraftComponentClassCache,
            "FortControllerComponent_Aircraft");

    if (AircraftComponentClass)
    {
        auto Component =
            PC->GetComponentByClass(
                AircraftComponentClass);

        if (PlayerAITryReadInAircraftBit(
                Component, bInAircraft) &&
            bInAircraft)
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

    // Same native function real players go through (Magnesium hooks it; we
    // call the original directly so the hook's inventory handling stays
    // exclusive to real player flows).
    if (!AFortPlayerControllerAthena::EnterAircraftOG)
    {
        AIDebugLogger::MissingFeature("EnterAircraft", "PlayerAI uses landing teleport fallback");
        return false;
    }

    auto CompClass =
        PlayerAIResolveCachedClass(
            PlayerAIAircraftComponentClassCache,
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

    // Actually flagged aboard (should not happen anymore - the EnterAircraft
    // hook skips PlayerAI - but old saves/edge versions): try the native
    // jump RPC first, some versions accept server-side calls.
    if (IsInAircraft(PC))
    {
        auto CompClass =
            PlayerAIResolveCachedClass(
                PlayerAIAircraftComponentClassCache,
                "FortControllerComponent_Aircraft");

        const UObject* Target = PC;

        if (CompClass)
        {
            auto Component =
                PC->GetComponentByClass(CompClass);

            if (Component)
                Target = Component;
        }

        auto Fn = Target->GetFunction("ServerAttemptAircraftJump");

        if (Fn)
            SafeCallNoArgs(Target, Fn);

        if (!IsInAircraft(PC) && PC->MyFortPawn)
            return true;
    }

    // Connectionless controllers are rejected by the native RPC on several
    // versions. Keep their already-valid pawn and clear only the aircraft
    // bookkeeping; the shared transport behavior safely teleports that pawn
    // to the aircraft and begins skydiving. RestartPlayer used to destroy or
    // orphan the warmup pawn here, causing the visible "zip then die" failure.
    ForceLeaveAircraft(PC);
    return PC->Pawn != nullptr || PC->MyFortPawn != nullptr;
}

// Clears a replicated "bInAircraft"-style bitfield on one object (same
// reflection pattern as DEFINE_BITFIELD_PROP, but by-name so it works on
// whichever class carries the flag on a version).
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

// Decodes only the narrow instruction form used by the legacy controller
// aircraft code:
//     cmp byte ptr [saved-this + disp32], 0
// Supporting the optional REX prefix and a no-index SIB keeps this independent
// of the compiler's choice of nonvolatile register without becoming a general
// purpose (and therefore ambiguous) instruction decoder.
static bool PlayerAITryDecodeCmpByteZero(
    const uint8* Code,
    size_t Remaining,
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
        const uint8 IndexRegister =
            ((Sib >> 3) & 7) + ((Rex & 0x02) ? 8 : 0);

        // An encoded index of four without REX.X means "no index".
        if (IndexRegister != 4)
            return false;

        BaseRegister =
            (Sib & 7) + ((Rex & 0x01) ? 8 : 0);
    }
    else
    {
        BaseRegister += (Rex & 0x01) ? 8 : 0;
    }

    if (Remaining - Cursor < sizeof(uint32) + 1)
        return false;

    uint32 Displacement = 0;
    memcpy(
        &Displacement,
        Code + Cursor,
        sizeof(Displacement));
    Cursor += sizeof(Displacement);

    if (Code[Cursor++] != 0)
        return false;

    Out.BaseRegister = BaseRegister;
    Out.Displacement = Displacement;
    Out.Size = Cursor;
    return true;
}

static bool PlayerAIFunctionSavesThisInRegister(
    const uint8* Code,
    size_t End,
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

        const uint8 Reg =
            ((ModRM >> 3) & 7) +
            ((Rex & 0x04) ? 8 : 0);
        const uint8 Rm =
            (ModRM & 7) +
            ((Rex & 0x01) ? 8 : 0);
        const uint8 Destination =
            Opcode == 0x8B ? Reg : Rm;
        const uint8 Source =
            Opcode == 0x8B ? Rm : Reg;

        // Windows x64 passes the controller ("this") in RCX. The compared
        // base must be the nonvolatile register into which this function
        // actually saved RCX, not merely another object with nearby bools.
        if (Source == 1 &&
            Destination == ExpectedRegister)
        {
            return true;
        }
    }

    return false;
}

static bool PlayerAIResolveLegacyAircraftExitLatch(
    AFortPlayerControllerAthena* PC)
{
    auto& Cache = PlayerAILegacyAircraftExitLatchCache;

    if (Cache.bResolved)
        return Cache.bSupported;

    Cache.bResolved = true;

    if (!PC)
        return false;

    auto ReadyProperty =
        PC->GetProperty("bReadyToStartMatch", 0x20000);

    if (!ReadyProperty)
    {
        AIDebugLogger::Error(
            "Transport",
            "legacy aircraft exit latch unresolved: bReadyToStartMatch was not reflected");
        return false;
    }

    const uint32 ReadyOffset = GetFromOffset<uint32>(
        ReadyProperty, Offsets::Offset_Internal);
    const uint8 ReadyMask =
        ReadyProperty->GetFieldMask();

    if (!ReadyMask || ReadyOffset >= 0x20000)
    {
        AIDebugLogger::Error(
            "Transport",
            "legacy aircraft exit latch unresolved: invalid ready field metadata");
        return false;
    }

    const uint64 EnterAircraft = FindEnterAircraft();
    constexpr size_t ScanSize = 0x600;
    const uint64 WarningReference =
        Memcury::Scanner::FindStringRef(
            L"EnterAircraft: [%s] is attempting to enter aircraft after having already exited.",
            true, 0,
            VersionInfo.FortniteVersion >= 19)
            .Get();

    if (!EnterAircraft ||
        WarningReference < EnterAircraft ||
        WarningReference >= EnterAircraft + ScanSize ||
        !SDK::MemReadable(
            (const void*)EnterAircraft, ScanSize))
    {
        AIDebugLogger::Error(
            "Transport",
            "legacy aircraft exit latch unresolved: EnterAircraft warning/code validation failed");
        return false;
    }

    const auto Code =
        reinterpret_cast<const uint8*>(EnterAircraft);
    const size_t WarningAt =
        (size_t)(WarningReference - EnterAircraft);
    uint32 ResolvedOffset = 0;
    int MatchCount = 0;

    for (size_t FirstAt = 0;
         FirstAt + 7 <= ScanSize;
         FirstAt++)
    {
        FPlayerAICmpByteZeroInstruction First{};

        if (!PlayerAITryDecodeCmpByteZero(
                Code + FirstAt,
                ScanSize - FirstAt,
                First))
        {
            continue;
        }

        // The private exit latch sits in the same small controller bool
        // cluster immediately before the reflected ready-to-start field.
        // Reflection is the per-build anchor; no Fortnite offset is baked in.
        if (First.Displacement == 0 ||
            First.Displacement >= ReadyOffset ||
            ReadyOffset - First.Displacement > 0x40 ||
            FirstAt >= WarningAt ||
            WarningAt - FirstAt > 0x100 ||
            !PlayerAIFunctionSavesThisInRegister(
                Code, FirstAt, First.BaseRegister))
        {
            continue;
        }

        const size_t DesiredPairEnd =
            FirstAt + 0x300;
        const size_t PairEnd =
            DesiredPairEnd < ScanSize
            ? DesiredPairEnd
            : ScanSize;
        bool bFoundPair = false;

        for (size_t SecondAt =
                 FirstAt + First.Size;
             SecondAt + 7 <= PairEnd;
             SecondAt++)
        {
            FPlayerAICmpByteZeroInstruction Second{};

            if (!PlayerAITryDecodeCmpByteZero(
                    Code + SecondAt,
                    ScanSize - SecondAt,
                    Second))
            {
                continue;
            }

            if (Second.BaseRegister ==
                    First.BaseRegister &&
                Second.Displacement + 1 ==
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

    // Multiple candidates would make a private-field write unsafe. The
    // expected legacy routine has exactly one [latch]/[latch-1] pair.
    if (MatchCount != 1 ||
        ResolvedOffset == 0)
    {
        AIDebugLogger::Error(
            "Transport",
            "legacy aircraft exit latch unresolved: expected one validated compare pair, found %d",
            MatchCount);
        return false;
    }

    Cache.ExitLatchOffset = ResolvedOffset;
    Cache.ReadyOffset = ReadyOffset;
    Cache.ReadyMask = ReadyMask;
    Cache.bSupported = true;

    AIDebugLogger::Log(
        "Transport",
        "resolved legacy aircraft exit latch relative to bReadyToStartMatch (%u bytes before)",
        ReadyOffset - ResolvedOffset);
    return true;
}

static bool PlayerAITryApplyLegacyAircraftExitLatch(
    AFortPlayerControllerAthena* PC)
{
    // This private two-byte state transition belongs only to the pre-Chapter
    // 2 controller path and only to PlayerAI entities tracked by this
    // subsystem. The machine-code/reflection resolver below is the layout
    // authority, so an unrelated late-loaded component class cannot suppress
    // the legacy transition.
    if (!PC ||
        VersionInfo.FortniteVersion >= 11.00 ||
        !PlayerAIManager::IsPlayerAI(PC) ||
        !PlayerAIResolveLegacyAircraftExitLatch(PC))
    {
        return false;
    }

    const auto& Cache =
        PlayerAILegacyAircraftExitLatchCache;
    auto ReadyAddress =
        reinterpret_cast<const uint8*>(PC) +
        Cache.ReadyOffset;
    auto ExitStateAddress =
        reinterpret_cast<uint8*>(PC) +
        Cache.ExitLatchOffset - 1;

    if (!SDK::MemReadable(ReadyAddress, 1) ||
        !SDK::MemReadable(ExitStateAddress, sizeof(uint16)) ||
        (*ReadyAddress & Cache.ReadyMask) == 0)
    {
        return false;
    }

    // Legacy ExitAircraft performs this as one word write: clear the
    // in-aircraft byte and set the adjacent "has exited" latch.
    uint16 CurrentState = 0;
    memcpy(
        &CurrentState,
        ExitStateAddress,
        sizeof(CurrentState));

    if (CurrentState == 0x0100)
        return true;

    const uint8 InAircraftByte =
        static_cast<uint8>(CurrentState & 0xFF);
    const uint8 HasExitedByte =
        static_cast<uint8>(CurrentState >> 8);

    // Both native fields are byte booleans. Unexpected contents indicate
    // that the dynamically inferred layout is not safe to mutate.
    if (InAircraftByte > 1 ||
        HasExitedByte > 1)
    {
        return false;
    }

    const uint16 ExitedState = 0x0100;
    memcpy(
        ExitStateAddress,
        &ExitedState,
        sizeof(ExitedState));
    return true;
}

static UFortControllerComponent_Aircraft*
PlayerAITryGetAircraftComponent(
    AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return nullptr;

    auto ComponentClass =
        PlayerAIResolveCachedClass(
            PlayerAIAircraftComponentClassCache,
            "FortControllerComponent_Aircraft");

    // Legacy controller-owned aircraft versions do not contain this class.
    // Return before doing a reflected function lookup for every lobby member.
    if (!ComponentClass)
        return nullptr;

    UFortControllerComponent_Aircraft* Result = nullptr;
    auto Function =
        PC->GetFunction("GetAircraftComponent");

    if (Function)
    {
        GPlayerAIGuardedNativeCallDepth++;

        __try
        {
            Result =
                PC->Call<UFortControllerComponent_Aircraft*>(
                    Function);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Result = nullptr;
        }

        GPlayerAIGuardedNativeCallDepth--;
    }

    if ((!PlayerAIIsLiveSupportObject(Result) ||
         !Result->IsA(ComponentClass)))
    {
        Result = (UFortControllerComponent_Aircraft*)
            PC->GetComponentByClass(ComponentClass);
    }

    return PlayerAIIsLiveSupportObject(Result) &&
        Result->IsA(ComponentClass)
        ? Result
        : nullptr;
}

bool VersionFeatureAdapter::MarkVirtualAircraftExited(
    AFortPlayerControllerAthena* PC)
{
    if (VersionInfo.FortniteVersion >= 11.00)
        return false;

    auto ControllerClass =
        PlayerAIResolveCachedClass(
            PlayerAIControllerClassCache,
            "FortPlayerControllerAthena");

    if (!ControllerClass ||
        !PlayerAIIsLiveSupportActor(PC) ||
        !PC->IsA(ControllerClass) ||
        !PlayerAIManager::IsPlayerAI(PC))
    {
        return false;
    }

    return PlayerAITryApplyLegacyAircraftExitLatch(PC);
}

static AFortPlayerPawnAthena*
PlayerAIRefreshControllerPawn(
    AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return nullptr;

    auto PawnClass =
        PlayerAIResolveCachedClass(
            PlayerAIPawnClassCache,
            "FortPlayerPawnAthena");
    auto IsLiveAthenaPawn =
        [PawnClass](AFortPlayerPawnAthena* Candidate)
        {
            return PawnClass &&
                PlayerAIIsLiveSupportActor(Candidate) &&
                Candidate->IsA(PawnClass);
        };
    AFortPlayerPawnAthena* Pawn = nullptr;

    if (IsLiveAthenaPawn(PC->Pawn) &&
        PC->Pawn->Controller == PC)
        Pawn = PC->Pawn;
    else if (IsLiveAthenaPawn(PC->MyFortPawn) &&
        PC->MyFortPawn->Controller == PC)
        Pawn = PC->MyFortPawn;
    else if (IsLiveAthenaPawn(PC->Pawn))
        Pawn = PC->Pawn;
    else if (IsLiveAthenaPawn(PC->MyFortPawn))
        Pawn = PC->MyFortPawn;
    else
        return nullptr;

    // KickFromAircraft can replace one controller pawn mirror before the
    // other. Reconcile them to the live possessed Athena pawn before the
    // caller begins skydiving.
    PC->Pawn = Pawn;
    PC->MyFortPawn = Pawn;
    return Pawn;
}

void VersionFeatureAdapter::ForceLeaveAircraft(AFortPlayerControllerAthena* PC)
{
    auto ControllerClass =
        PlayerAIResolveCachedClass(
            PlayerAIControllerClassCache,
            "FortPlayerControllerAthena");

    if (!ControllerClass ||
        !PlayerAIIsLiveSupportActor(PC) ||
        !PC->IsA(ControllerClass))
        return;

    UFortControllerComponent_Aircraft* AircraftComponent =
        PlayerAITryGetAircraftComponent(PC);
    bool bWasAboard = IsInAircraft(PC);
    bool bMirrorAboard = false;

    if (AircraftComponent &&
        PlayerAITryReadInAircraftBit(
            AircraftComponent, bMirrorAboard))
    {
        bWasAboard |= bMirrorAboard;
    }
    if (PlayerAITryReadInAircraftBit(
            PC, bMirrorAboard))
        bWasAboard |= bMirrorAboard;
    if (PC->PlayerState &&
        PlayerAITryReadInAircraftBit(
            PC->PlayerState, bMirrorAboard))
        bWasAboard |= bMirrorAboard;

    auto Pawn = PlayerAIRefreshControllerPawn(PC);
    bool bCleared = PlayerAITryClearInAircraftBit(
        AircraftComponent);
    bCleared |= PlayerAITryClearInAircraftBit(PC);

    if (PC->PlayerState)
        bCleared |= PlayerAITryClearInAircraftBit(
            PC->PlayerState);

    const bool bExitLatchApplied =
        PlayerAITryApplyLegacyAircraftExitLatch(PC);

    PC->ForceNetUpdate();

    if (PC->PlayerState)
        PC->PlayerState->ForceNetUpdate();

    if (Pawn)
        Pawn->ForceNetUpdate();

    AIDebugLogger::Verbose(
        "Transport",
        "force leave aircraft: was aboard: %d, pawn: %d, flag %s, legacy exit latch: %d, still aboard: %d",
        bWasAboard ? 1 : 0, Pawn ? 1 : 0,
        bCleared ? "cleared" : "not found",
        bExitLatchApplied ? 1 : 0,
        IsInAircraft(PC) ? 1 : 0);
}

static AFortPlayerControllerAthena*
PlayerAIGetPawnController(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn || !PlayerAIIsLiveSupportActor(Pawn->Controller))
        return nullptr;

    auto ControllerClass =
        PlayerAIResolveCachedClass(
            PlayerAIControllerClassCache,
            "FortPlayerControllerAthena");

    return ControllerClass &&
        Pawn->Controller->IsA(ControllerClass)
        ? (AFortPlayerControllerAthena*)Pawn->Controller
        : nullptr;
}

static bool PlayerAIHasObservedSkydiveState(
    AFortPlayerPawnAthena* Pawn, bool bWasInAircraft)
{
    if (!Pawn)
        return false;

    if ((Pawn->HasbIsSkydiving() &&
         Pawn->bIsSkydiving) ||
        (Pawn->HasbIsSkydivingFromBus() &&
         Pawn->bIsSkydivingFromBus))
    {
        return true;
    }

    auto PC = PlayerAIGetPawnController(Pawn);

    if (!PC)
        return false;

    const bool bNowInAircraft =
        VersionFeatureAdapter::IsInAircraft(PC);

    if (bWasInAircraft && !bNowInAircraft)
        return true;

    // The replicated pawn flags can trail the authoritative movement state
    // by a frame. Observed falling after the controller is off the aircraft
    // is a bounded grace signal; ProcessEvent success alone is never enough.
    bool bGrounded = false;
    return !bNowInAircraft &&
        VersionFeatureAdapter::TryIsPawnGrounded(
            Pawn, bGrounded) &&
        !bGrounded;
}

// Starts the native skydive (BeginSkydiving(bFromAircraft=true)) through a
// sized parameter buffer - the engine then owns descent, glider deploy and
// landing exactly like a real bus jumper.
bool VersionFeatureAdapter::TryBeginSkydiving(AFortPlayerPawnAthena* Pawn)
{
    auto PawnClass =
        PlayerAIResolveCachedClass(
            PlayerAIPawnClassCache,
            "FortPlayerPawnAthena");

    if (!PawnClass ||
        !PlayerAIIsLiveSupportActor(Pawn) ||
        !Pawn->IsA(PawnClass))
        return false;

    auto Fn = Pawn->GetFunction("BeginSkydiving");

    if (!PlayerAIIsLiveSupportObject(Fn))
    {
        AIDebugLogger::MissingFeature("BeginSkydivingForPlayerAI",
            "no BeginSkydiving on this version - PlayerAI lands by direct placement");
        return false;
    }

    const auto Params = Fn->GetParamsNamed();
    const bool bEncryptedParameterMetadata =
        VersionInfo.FortniteVersion >= 32.00;
    const size_t BufferSize =
        bEncryptedParameterMetadata
        ? 0x1000
        : Params.Size > 0
        ? (size_t)Params.Size
        : 1;

    if (BufferSize > 0x1000)
    {
        AIDebugLogger::MissingFeature(
            "BeginSkydivingForPlayerAI",
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

        if (!bEncryptedParameterMetadata &&
            (Param.PropertyFlags & 0x400) == 0 &&
            Param.ElementSize == 1 &&
            FallbackBoolOffset == -1)
        {
            FallbackBoolOffset = (int)Param.Offset;
        }
    }

    if (BoolOffset == -1)
        BoolOffset = FallbackBoolOffset;

    // FN32+ encrypts ElementSize/PropertyFlags/PropertiesSize but preserves
    // the decrypted named offset. Do not guess a positional boolean there.
    if ((bEncryptedParameterMetadata &&
         BoolOffset < 0) ||
        BoolOffset >= (int)BufferSize)
    {
        AIDebugLogger::MissingFeature(
            "BeginSkydivingForPlayerAI",
            "bFromAircraft could not be resolved inside the guarded buffer");
        return false;
    }

    std::vector<uint8_t> Buffer(BufferSize, 0);

    if (BoolOffset >= 0)
        Buffer[(size_t)BoolOffset] = 1; // bFromAircraft = true

    auto PC = PlayerAIGetPawnController(Pawn);
    const bool bWasInAircraft =
        PC && IsInAircraft(PC);

    if (!PlayerAIGuardedProcessEvent(Pawn, Fn, Buffer.data()))
        return false;

    return PlayerAIHasObservedSkydiveState(
        Pawn, bWasInAircraft);
}

// ---- Movement / world -----------------------------------------------------------

// Isolates the native ground trace: if it faults on a version (observed as
// a null read inside the engine), ground tracing is disabled for the session
// and the AI falls back to flat movement instead of crashing the gameserver.
// (Kept free of unwindable C++ objects so SEH is allowed here.)
static bool PlayerAITryGroundTrace(UWorld* World, AFortPlayerPawnAthena* IgnorePawn, const FVector& Near, FVector& OutGround)
{
    GPlayerAIGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        OutGround = UFortKismetLibrary::FindGroundLocationAt(World, IgnorePawn, FVector(Near), 10000.f, -10000.f, FName());
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GPlayerAIGuardedNativeCallDepth--;
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

    // Needs a real success rate, not just "did not crash": some versions
    // return zero vectors from the trace most of the time.
    if (GroundTraceCalls >= 20 &&
        (int64_t)GroundTraceHits * 10 <
        (int64_t)GroundTraceCalls * 3)
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

FVector VersionFeatureAdapter::FindGroundLocation(const FVector& Near, bool& bOutFound, AFortPlayerPawnAthena* IgnorePawn)
{
    bOutFound = false;

    if (!PlayerAIUpdateGroundTraceReliability())
        return Near;

    // Landing, recovery, and aircraft callbacks can converge for an entire
    // lobby in one TickFlush. Keep the native trace work bounded globally;
    // callers retain their protected airborne state and retry next tick.
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
    const bool bFoundGround =
        Ground.X != 0.f || Ground.Y != 0.f || Ground.Z != 0.f;

    if (bFoundGround)
        GroundTraceHits++;

    // Apply the cutoff on the trace that crosses the sample threshold.
    // This prevents callers that do not query IsGroundTraceReliable first
    // from continuing to issue native probes for the rest of the session.
    if (!PlayerAIUpdateGroundTraceReliability() || !bFoundGround)
        return Near;

    bOutFound = true;
    return Ground;
}

bool VersionFeatureAdapter::TryResolveGroundedLandingSpot(
    const FVector& Desired,
    AFortPlayerPawnAthena* IgnorePawn,
    FVector& OutSpot)
{
    OutSpot = FVector{};

    if (!std::isfinite((double)Desired.X) ||
        !std::isfinite((double)Desired.Y) ||
        !std::isfinite((double)Desired.Z))
    {
        return false;
    }

    bool bFound = false;
    auto Ground =
        FindGroundLocation(Desired, bFound, IgnorePawn);

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

    if (!PlayerAIUpdateGroundTraceReliability() ||
        PlayerAIGroundTraceBudgetRemaining <= 0)
    {
        return false;
    }

    constexpr uint32_t RingCount =
        (uint32_t)(sizeof(Radii) / sizeof(Radii[0]));
    constexpr uint32_t DirectionCount =
        (uint32_t)(sizeof(Directions) /
                   sizeof(Directions[0]));
    constexpr uint32_t ProbeCount =
        RingCount * DirectionCount;

    // The desired location consumed the first possible native probe. Rotate
    // one fallback through the old spiral across calls, retaining coverage
    // while hard-capping this resolver at two native probes per invocation.
    const uint32_t ProbeIndex =
        PlayerAILandingProbeCursor++ % ProbeCount;
    const uint32_t Ring = ProbeIndex / DirectionCount;
    const uint32_t Step = ProbeIndex % DirectionCount;
    const uint32_t Direction =
        (Step + Ring * 3) % DirectionCount;
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

static bool PlayerAITryReadWalkingMovementMode(
    UCharacterMovementComponent* Movement,
    bool& OutGrounded)
{
    bool bRead = false;
    GPlayerAIGuardedNativeCallDepth++;

    __try
    {
        const uint32 Offset =
            Movement->GetOffset("MovementMode");

        if (Offset != (uint32)-1 &&
            Offset < 0x10000 &&
            SDK::MemReadable(
                (const uint8_t*)Movement + Offset, 1))
        {
            const uint8_t Mode =
                GetFromOffset<uint8_t>(Movement, Offset);

            // EMovementMode is stable in UE4/UE5: Walking=1 and
            // NavWalking=2. Reject corrupt/out-of-range reads rather than
            // interpreting Flying/Custom/None as grounded.
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

    GPlayerAIGuardedNativeCallDepth--;
    return bRead;
}

bool VersionFeatureAdapter::TryIsPawnGrounded(
    AFortPlayerPawnAthena* Pawn, bool& OutGrounded)
{
    OutGrounded = false;
    auto PawnClass =
        PlayerAIResolveCachedClass(
            PlayerAIPawnClassCache,
            "FortPlayerPawnAthena");

    if (!PawnClass ||
        !PlayerAIIsLiveSupportActor(Pawn) ||
        !Pawn->IsA(PawnClass) ||
        !Pawn->HasCharacterMovement())
    {
        return false;
    }

    auto Movement = Pawn->CharacterMovement;
    auto MovementClass =
        PlayerAIResolveCachedClass(
            PlayerAIMovementComponentClassCache,
            "CharacterMovementComponent");

    if (!MovementClass ||
        !PlayerAIIsLiveSupportObject(Movement) ||
        !Movement->IsA(MovementClass))
    {
        return false;
    }

    auto Function =
        Movement->GetFunction("IsMovingOnGround");

    if (!PlayerAIIsLiveSupportObject(Function))
        return PlayerAITryReadWalkingMovementMode(
            Movement, OutGrounded);

    const auto Params = Function->GetParamsNamed();
    const bool bEncryptedParameterMetadata =
        VersionInfo.FortniteVersion >= 32.00;
    const size_t BufferSize =
        bEncryptedParameterMetadata
        ? 0x1000
        : Params.Size > 0
        ? (size_t)Params.Size
        : 1;

    if (BufferSize > 0x1000)
        return PlayerAITryReadWalkingMovementMode(
            Movement, OutGrounded);

    int ReturnOffset = -1;

    for (const auto& Param : Params.NameOffsetMap)
    {
        if (Param.Name == "ReturnValue")
        {
            if (!bEncryptedParameterMetadata &&
                (Param.ElementSize != 1 ||
                 (Param.PropertyFlags & 0x400) == 0))
            {
                return PlayerAITryReadWalkingMovementMode(
                    Movement, OutGrounded);
            }

            ReturnOffset = (int)Param.Offset;
            continue;
        }

        // IsMovingOnGround must remain a no-argument query. On FN32 the
        // flags are encrypted, so any additional reflected field is
        // conservatively treated as an unsupported schema.
        if (bEncryptedParameterMetadata ||
            (Param.PropertyFlags & 0x80) != 0)
        {
            return PlayerAITryReadWalkingMovementMode(
                Movement, OutGrounded);
        }
    }

    if (ReturnOffset < 0 ||
        ReturnOffset >= (int)BufferSize)
    {
        return PlayerAITryReadWalkingMovementMode(
            Movement, OutGrounded);
    }

    std::vector<uint8_t> Buffer(BufferSize, 0);

    if (!PlayerAIGuardedProcessEvent(
            Movement, Function, Buffer.data()))
    {
        return PlayerAITryReadWalkingMovementMode(
            Movement, OutGrounded);
    }

    OutGrounded =
        Buffer[(size_t)ReturnOffset] != 0;
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
    // Gliding exists whenever an aircraft/skydiving flow exists; the pawn
    // handles it natively after an aircraft jump. Used for logging only.
    return GetAircraft() != nullptr;
}

// ---- Safe zone / storm ----------------------------------------------------------------

static bool PlayerAIIsLiveSafeZoneIndicator(
    AFortSafeZoneIndicator* Indicator)
{
    if (!PlayerAIIsLiveSupportActor(Indicator))
        return false;

    auto IndicatorClass =
        PlayerAIResolveCachedClass(
            PlayerAISafeZoneIndicatorClassCache,
            "FortSafeZoneIndicator");

    return IndicatorClass &&
        Indicator->IsA(IndicatorClass);
}

static AFortSafeZoneIndicator*
PlayerAIResolveSafeZoneIndicator()
{
    if (auto PhaseLogic = PlayerAIResolvePhaseLogic())
    {
        if (PhaseLogic->HasSafeZoneIndicator())
        {
            auto Indicator =
                PhaseLogic->SafeZoneIndicator;

            if (PlayerAIIsLiveSafeZoneIndicator(
                    Indicator))
            {
                return Indicator;
            }
        }
    }

    auto GameMode =
        VersionFeatureAdapter::GetGameMode();

    if (!PlayerAIIsLiveSupportActor(GameMode) ||
        !GameMode->HasSafeZoneIndicator())
    {
        return nullptr;
    }

    auto Indicator = GameMode->SafeZoneIndicator;
    return PlayerAIIsLiveSafeZoneIndicator(Indicator)
        ? Indicator
        : nullptr;
}

bool VersionFeatureAdapter::TryGetSafeZone(FVector& OutCenter, float& OutRadius)
{
    OutCenter = FVector{};
    OutRadius = 0.f;
    auto Indicator =
        PlayerAIResolveSafeZoneIndicator();

    if (!Indicator)
        return false;

    if (Indicator->HasNextCenter() && Indicator->HasNextRadius())
    {
        OutCenter = Indicator->NextCenter;
        OutRadius = Indicator->NextRadius;
        return std::isfinite((double)OutCenter.X) &&
            std::isfinite((double)OutCenter.Y) &&
            std::isfinite((double)OutCenter.Z) &&
            std::isfinite((double)OutRadius) &&
            OutRadius > 0.f;
    }

    // Old builds.
    if (Indicator->HasLastCenter() && Indicator->HasRadius())
    {
        OutCenter = Indicator->LastCenter;
        OutRadius = Indicator->Radius;
        return std::isfinite((double)OutCenter.X) &&
            std::isfinite((double)OutCenter.Y) &&
            std::isfinite((double)OutCenter.Z) &&
            std::isfinite((double)OutRadius) &&
            OutRadius > 0.f;
    }

    return false;
}

static bool PlayerAITryIsInCurrentSafeZone(
    const UObject* Owner, const FVector& Location,
    bool& OutInside)
{
    if (!PlayerAIIsLiveSupportObject(Owner))
        return false;

    auto Function =
        Owner->GetFunction("IsInCurrentSafeZone");

    if (!PlayerAIIsLiveSupportObject(Function))
        return false;

    bool bCalled = false;
    GPlayerAIGuardedNativeCallDepth++;

    __try
    {
        OutInside = Owner->Call<bool>(
            Function, Location, false);
        bCalled = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    GPlayerAIGuardedNativeCallDepth--;
    return bCalled;
}

bool VersionFeatureAdapter::IsInsideSafeZone(const FVector& Location)
{
    auto GameMode = GetGameMode();
    auto PhaseLogic = PlayerAIResolvePhaseLogic();
    auto Indicator =
        PlayerAIResolveSafeZoneIndicator();

    const bool bHasComponentIndicator =
        PhaseLogic && PhaseLogic->HasSafeZoneIndicator() &&
        PhaseLogic->SafeZoneIndicator == Indicator;
    const bool bHasGameModeIndicator =
        PlayerAIIsLiveSupportActor(GameMode) &&
        GameMode->HasSafeZoneIndicator() &&
        GameMode->SafeZoneIndicator == Indicator;

    if (!Indicator)
        return true; // no storm yet - everywhere is safe

    bool bInside = true;

    if (bHasComponentIndicator &&
        PlayerAITryIsInCurrentSafeZone(
            PhaseLogic, Location, bInside))
    {
        return bInside;
    }

    if (bHasGameModeIndicator &&
        PlayerAITryIsInCurrentSafeZone(
            GameMode, Location, bInside))
    {
        return bInside;
    }

    // Fallback: distance check against the target circle.
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
    auto Indicator =
        PlayerAIResolveSafeZoneIndicator();

    if (!Indicator ||
        !Indicator->HasSafeZoneFinishShrinkTime())
    {
        return false;
    }

    const float Finish =
        Indicator->SafeZoneFinishShrinkTime;
    return std::isfinite((double)Finish) &&
        Finish > 1.f && TimeSeconds >= Finish;
}

float VersionFeatureAdapter::GetStormDamagePerSecond()
{
    auto GameMode = GetGameMode();
    auto Indicator =
        PlayerAIResolveSafeZoneIndicator();

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

    // Version specific damage info when available. Percentage semantics vary
    // between versions, so the result is clamped to a sane per-second range -
    // the fallback must never insta-melt a full-health PlayerAI.
    if (Indicator && Indicator->HasCurrentDamageInfo() &&
        FFortSafeZoneDamageInfo::HasDamage())
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

// NOTE: version-specific damage info can be percentage based with differing
// semantics per version - the clamp keeps the fallback from ever melting a
// full-health PlayerAI in seconds.

// ---- DBNO --------------------------------------------------------------------------

bool VersionFeatureAdapter::SupportsDBNO()
{
    auto GameState = GetGameState();

    if (!GameState)
        return false;

    if (GameState->HasbDBNOEnabledForGameMode())
        return GameState->bDBNOEnabledForGameMode;

    return false;
}

// ---- Death ---------------------------------------------------------------------------

bool VersionFeatureAdapter::KillPawn(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* KillerPC, AActor* DamageCauser)
{
    if (!Pawn)
        return false;

    // Native death pipeline: ForceKill produces a real death report, which
    // flows through Magnesium's existing elimination handling
    // (ClientOnPawnDied -> kill credit, kill feed, alive counts, placement,
    // win conditions). TODO: connect this to the Magnesium damage system if
    // a gameplay-effect based kill is preferred on newer versions.
    auto ForceKillFn = Pawn->GetFunction("ForceKill");

    if (ForceKillFn)
    {
        FGameplayTag DeathTag{};
        Pawn->Call<void>(ForceKillFn, DeathTag, KillerPC, DamageCauser);
        return true;
    }

    AIDebugLogger::MissingFeature("ForceKill", "using direct health zeroing (kill credit may be reduced)");
    Pawn->SetHealth(0.f);
    Pawn->ForceNetUpdate();
    return true;
}

// ---- Cosmetics ----------------------------------------------------------------------------

static int SkinCacheAttempts = 0;
static std::vector<UAthenaCharacterItemDefinition*> CachedSkins;
static bool bCosmeticPathLoadDisabled = false;
static bool bSoftCosmeticResolveDisabled = false;
static bool bKnownSkinsLoaded = false;
static bool bLoadedSkinScanCompleted = false;
static int KnownSkinScanCursor = 0;
static int LoadedSkinScanCursor = 0;
static int LoadedSkinScanLimit = 0;
static bool bDefaultCommandoResolved = false;
static const UObject* PlayerAIDefaultCommando = nullptr;
static bool bDefaultPartsResolved = false;
static const UObject* PlayerAIDefaultHead = nullptr;
static const UObject* PlayerAIDefaultBody = nullptr;
static const UObject* PlayerAIDefaultBackpack = nullptr;

// Resident-only cosmetic lookup. FindObject normally falls through to
// StaticLoadObject, which can block TickFlush on package IO. Optional AI
// cosmetics must never load a package from the spawn/tick path.
static const UObject* PlayerAITryFindLoadedCosmetic(
    const wchar_t* Path, const UClass* Class)
{
    if (bCosmeticPathLoadDisabled || !Path || !Class ||
        !Offsets::StaticFindObject)
        return nullptr;

    GPlayerAIGuardedNativeCallDepth++;

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

    GPlayerAIGuardedNativeCallDepth--;

    if (bFaulted)
    {
        bCosmeticPathLoadDisabled = true;
        AIDebugLogger::MissingFeature("CosmeticAssetLoading",
            "resident cosmetic lookup faulted and was disabled");
    }

    return Result;
}

// Resolve only an already-loaded weak object. InternalGet falls through to
// synchronous package loading for unresolved soft paths, which is unsafe in
// a server tick and was the main first-AI freeze.
static const UObject* PlayerAITryResolveLoadedSoftObject(
    FSoftObjectPtr& SoftObject, const UClass* Class)
{
    if (bSoftCosmeticResolveDisabled || !Class)
        return nullptr;

    GPlayerAIGuardedNativeCallDepth++;
    const UObject* Result = nullptr;
    bool bFaulted = false;

    __try
    {
        Result = SoftObject.Get();

        if (Result &&
            (!PlayerAIIsLiveSupportObject(Result) ||
             !Result->IsA(Class)))
        {
            Result = nullptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Result = nullptr;
        bFaulted = true;
    }

    GPlayerAIGuardedNativeCallDepth--;

    if (bFaulted)
    {
        bSoftCosmeticResolveDisabled = true;
        AIDebugLogger::MissingFeature(
            "SoftCosmeticResolution",
            "resident soft cosmetic lookup faulted and was disabled");
    }

    return Result;
}

// Isolates the native character customization call. On some builds the
// native routine faults for server-side (connectionless) player states;
// PlayerAI must never crash the gameserver, so the first failure disables
// the native path for this session. Character parts still replicate through
// the player state, so clients keep rendering the AI outfit either way.
// (Kept free of C++ objects so SEH is allowed here.)
static bool PlayerAITryNativeCustomization(uint64_t NativeFn, AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    GPlayerAIGuardedNativeCallDepth++;
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

    GPlayerAIGuardedNativeCallDepth--;
    return bOk;
}

static bool PlayerAITryUpdateCharacterPartsVisualization(
    AFortPlayerStateAthena* PlayerState)
{
    if (!PlayerState)
        return false;

    GPlayerAIGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        UFortKismetLibrary::
            UpdatePlayerCustomCharacterPartsVisualization(PlayerState);
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GPlayerAIGuardedNativeCallDepth--;
    return bOk &&
        UFortKismetLibrary::
        UpdatePlayerCustomCharacterPartsVisualization__Ptr != nullptr;
}

// Writes character part pointers (indexed by EFortCustomPartType) into the
// player state, clearing unused slots so a replacement skin cannot retain
// pieces of the previous one. Handles both the struct based
// CharacterParts layout of newer versions and the flat array of older ones.
static bool PlayerAIWriteCharacterParts(AFortPlayerStateAthena* PlayerState, const UObject* Parts[6])
{
    static auto NewStyleCharacterPartsOffset = PlayerState->GetOffset("CharacterParts", 0x100000);

    const UObject** Target = nullptr;
    const wchar_t* DirtyProperty = L"CharacterParts";

    if (NewStyleCharacterPartsOffset == -1)
    {
        static auto CharacterPartsOff =
            PlayerState->GetOffset("CharacterParts");
        static auto LocalCharacterPartsOff =
            PlayerState->GetOffset("LocalCharacterParts");
        const auto PartsOff = CharacterPartsOff != -1
            ? CharacterPartsOff
            : LocalCharacterPartsOff;

        if (PartsOff == -1)
        {
            AIDebugLogger::MissingFeature("CharacterParts", "PlayerAI keeps engine default appearance");
            return false;
        }

        DirtyProperty = CharacterPartsOff != -1
            ? L"CharacterParts"
            : L"LocalCharacterParts";
        Target = GetFromOffset<const UObject* [0x6]>(
            PlayerState, PartsOff);
    }
    else
    {
        static auto PartsStruct = FindStruct("CustomCharacterParts");

        if (!PartsStruct)
        {
            AIDebugLogger::MissingFeature("CustomCharacterParts", "PlayerAI keeps engine default appearance");
            return false;
        }

        static auto PartsOffset = PartsStruct->GetOffset("Parts");
        static auto CharacterPartsOff = PlayerState->GetOffset("CharacterParts");

        if (PartsOffset == -1 || CharacterPartsOff == -1)
        {
            AIDebugLogger::MissingFeature("CustomCharacterParts.Parts", "PlayerAI keeps engine default appearance");
            return false;
        }

        auto CharacterPartsPtr = (PBYTE)PlayerState + CharacterPartsOff;
        Target = GetFromOffset<const UObject* [0x6]>(CharacterPartsPtr, PartsOffset);
    }

    for (int i = 0; i < 6; i++)
        Target[i] = Parts[i];

    VersionFeatureAdapter::MarkReplicatedPropertyDirty(
        PlayerState, DirtyProperty);
    return true;
}

static void PlayerAIWriteHeroType(
    AFortPlayerStateAthena* PlayerState,
    const UObject* HeroType)
{
    if (!PlayerState || !PlayerState->HasHeroType())
        return;

    PlayerState->HeroType = HeroType;
    VersionFeatureAdapter::MarkReplicatedPropertyDirty(
        PlayerState, L"HeroType");
}

static void PlayerAIWriteGender(
    AFortPlayerStateAthena* PlayerState,
    EFortCustomGender Gender)
{
    if (!PlayerState)
        return;

    auto GenderOffset = PlayerState->GetOffset("Gender");
    const wchar_t* DirtyProperty = L"Gender";

    if (GenderOffset == (uint32)-1)
    {
        GenderOffset =
            PlayerState->GetOffset("CharacterGender");
        DirtyProperty = L"CharacterGender";
    }

    if (GenderOffset == (uint32)-1 ||
        GenderOffset >= 0x10000)
    {
        return;
    }

    GetFromOffset<EFortCustomGender>(
        PlayerState, GenderOffset) = Gender;
    VersionFeatureAdapter::MarkReplicatedPropertyDirty(
        PlayerState, DirtyProperty);
}

// Applies parts through the pawn's own native part selection when a pawn
// exists - the most reliable application path across versions (native
// replication + pawn visuals), used on top of the player state write.
static void PlayerAIChoosePartsOnPawn(AFortPlayerPawnAthena* Pawn, const UObject* Parts[6])
{
    if (!Pawn)
        return;

    for (int i = 0; i < 6; i++)
    {
        if (Parts[i])
            Pawn->ServerChoosePart((uint8)i, Parts[i]);
    }
}

// Shared cosmetics finish: replicate the parts and run the native
// customization polish (fault guarded, disabled after the first fault).
static void PlayerAIFinishCosmetics(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    const char* Notifications[] =
    {
        "OnRep_HeroType",
        "OnRep_CharacterParts",
        "OnRep_CharacterData",
        "OnRep_CharacterGender",
        "OnRep_Gender",
    };

    for (auto Name : Notifications)
    {
        if (auto Notification = PlayerState->GetFunction(Name))
            VersionFeatureAdapter::SafeCallNoArgs(
                PlayerState, Notification);
    }

    static bool bNativeCustomizationDisabled = false;
    const bool bVisualizationUpdated =
        PlayerAITryUpdateCharacterPartsVisualization(PlayerState);

    // Match the normal player customization path: the reflected Kismet
    // visualizer is authoritative when it exists. Only use the raw native
    // fallback when that API is absent or rejected.
    const bool bFortnite250 =
        VersionInfo.FortniteVersion >= 2.49 &&
        VersionInfo.FortniteVersion < 2.51;

    if (bFortnite250 && Pawn)
    {
        // 2.50's profile-driven native customization overwrites the valid
        // directly selected parts on a synthetic PlayerState.
        if (auto OnRepPlayerState =
                Pawn->GetFunction("OnRep_PlayerState"))
        {
            VersionFeatureAdapter::SafeCallNoArgs(
                Pawn, OnRepPlayerState);
        }
    }
    else if (!bVisualizationUpdated &&
        ApplyCharacterCustomization &&
        Pawn && !bNativeCustomizationDisabled)
    {
        if (!PlayerAITryNativeCustomization(ApplyCharacterCustomization, PlayerState, Pawn))
        {
            bNativeCustomizationDisabled = true;
            AIDebugLogger::MissingFeature("NativeCharacterCustomizationForPlayerAI",
                "native customization call faulted and was disabled - parts replicate via the player state");
        }
    }

    PlayerState->FlushNetDormancy();
    PlayerState->ForceNetUpdate();

    if (Pawn)
    {
        Pawn->FlushNetDormancy();
        Pawn->ForceNetUpdate();
    }
}

void VersionFeatureAdapter::ApplyDefaultCosmetics(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    if (!PlayerState)
        return;

    if (bDefaultCommandoResolved &&
        PlayerAIDefaultCommando &&
        !PlayerAIIsLiveSupportObject(PlayerAIDefaultCommando))
    {
        bDefaultCommandoResolved = false;
        PlayerAIDefaultCommando = nullptr;
    }

    if (!bDefaultCommandoResolved)
    {
        bDefaultCommandoResolved = true;
        PlayerAIDefaultCommando = PlayerAITryFindLoadedCosmetic(
            L"/Game/Athena/Heroes/HID_001_Athena_Commando_F.HID_001_Athena_Commando_F",
            UFortHeroType::StaticClass());

        // This fallback is absent on 2.50. Resolve it only when the original
        // HID is unavailable so old builds never load a known-missing path.
        if (!PlayerAIDefaultCommando)
        {
            PlayerAIDefaultCommando = PlayerAITryFindLoadedCosmetic(
                L"/Game/Athena/Heroes/HID_Commando_Athena_01.HID_Commando_Athena_01",
                UFortHeroType::StaticClass());
        }
    }

    if (bDefaultPartsResolved &&
        ((PlayerAIDefaultHead &&
          !PlayerAIIsLiveSupportObject(PlayerAIDefaultHead)) ||
         (PlayerAIDefaultBody &&
          !PlayerAIIsLiveSupportObject(PlayerAIDefaultBody)) ||
         (PlayerAIDefaultBackpack &&
          !PlayerAIIsLiveSupportObject(PlayerAIDefaultBackpack))))
    {
        bDefaultPartsResolved = false;
        PlayerAIDefaultHead = nullptr;
        PlayerAIDefaultBody = nullptr;
        PlayerAIDefaultBackpack = nullptr;
    }

    if (!bDefaultPartsResolved)
    {
        bDefaultPartsResolved = true;
        PlayerAIDefaultHead = PlayerAITryFindLoadedCosmetic(
            L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1",
            UCustomCharacterPart::StaticClass());
        PlayerAIDefaultBody = PlayerAITryFindLoadedCosmetic(
            L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01",
            UCustomCharacterPart::StaticClass());
        PlayerAIDefaultBackpack = PlayerAITryFindLoadedCosmetic(
            L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack",
            UCustomCharacterPart::StaticClass());
    }

    auto HeroClass = UFortHeroType::StaticClass();
    auto PartClass = UCustomCharacterPart::StaticClass();

    if (!HeroClass || !PartClass ||
        !PlayerAIIsLiveSupportObject(
            PlayerAIDefaultCommando) ||
        !PlayerAIDefaultCommando->IsA(HeroClass) ||
        !PlayerAIIsLiveSupportObject(
            PlayerAIDefaultHead) ||
        !PlayerAIDefaultHead->IsA(PartClass) ||
        !PlayerAIIsLiveSupportObject(
            PlayerAIDefaultBody) ||
        !PlayerAIDefaultBody->IsA(PartClass))
    {
        AIDebugLogger::MissingFeature(
            "DefaultCharacterParts",
            "complete default cosmetics unavailable; PlayerAI keeps its engine appearance");
        return;
    }

    const UObject* Parts[6] =
    {
        PlayerAIDefaultHead,
        PlayerAIDefaultBody,
        nullptr,
        PlayerAIDefaultBackpack,
        nullptr,
        nullptr,
    };

    if (PlayerAIWriteCharacterParts(PlayerState, Parts))
    {
        PlayerAIWriteHeroType(
            PlayerState, PlayerAIDefaultCommando);
        PlayerAIWriteGender(
            PlayerState, EFortCustomGender::Female);
        PlayerAIChoosePartsOnPawn(Pawn, Parts);
        PlayerAIFinishCosmetics(PlayerState, Pawn);
    }
}

// ---- Random skins from the hosted build ---------------------------------------------

// Known character definitions with stable names since the early seasons.
// These are probed resident-only in small batches; an unloaded or absent CID
// is skipped instead of forcing package IO from the server tick.
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

static void PlayerAITickKnownSkinDiscovery(int Budget)
{
    if (bKnownSkinsLoaded || Budget <= 0)
        return;

    auto AddSkin = [&](UAthenaCharacterItemDefinition* CID)
        {
            if (!CID || !CID->HasHeroDefinition() || !CID->HeroDefinition)
                return;

            for (auto Existing : CachedSkins)
                if (Existing == CID)
                    return;

            CachedSkins.push_back(CID);
        };

    const int Count =
        (int)(sizeof(PlayerAIKnownSkinNames) /
              sizeof(PlayerAIKnownSkinNames[0]));
    const int End = (std::min)(
        KnownSkinScanCursor + Budget, Count);

    for (int Index = KnownSkinScanCursor;
         Index < End; Index++)
    {
        if (bCosmeticPathLoadDisabled)
            break;

        auto Name = PlayerAIKnownSkinNames[Index];
        UEAllocatedWString Path = UEAllocatedWString(L"/Game/Athena/Items/Cosmetics/Characters/") + Name + L"." + Name;
        auto CID = (UAthenaCharacterItemDefinition*)
            PlayerAITryFindLoadedCosmetic(
                Path.c_str(),
                UAthenaCharacterItemDefinition::StaticClass());
        AddSkin(CID);
    }

    KnownSkinScanCursor = End;

    if (KnownSkinScanCursor >= Count ||
        bCosmeticPathLoadDisabled)
    {
        bKnownSkinsLoaded = true;
        AIDebugLogger::Log(
            "Cosmetics",
            "%d resident known skins discovered for PlayerAI",
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

    const int End = (std::min)(
        LoadedSkinScanCursor + Budget,
        LoadedSkinScanLimit);

    for (int i = LoadedSkinScanCursor;
         i < End && i < TUObjectArray::Num(); i++)
    {
        auto Object = TUObjectArray::GetObjectByIndex(i);

        if (!PlayerAIIsLiveSupportObject(Object) ||
            Object->IsDefaultObject() ||
            !Object->IsA<UAthenaCharacterItemDefinition>())
            continue;

        auto CID = (UAthenaCharacterItemDefinition*)Object;

        if (!CID->HasHeroDefinition() || !CID->HeroDefinition)
            continue;

        auto RawName = CID->Name.ToString();
        std::string Name(RawName.c_str());

        // Player CIDs only: retain whatever this hosted build has already
        // loaded, including newer naming schemes, while excluding known
        // NPC/placeholder families.
        const bool bValid = Name.find("CID_") != std::string::npos &&
            Name.find("CID_NPC") == std::string::npos &&
            Name.find("CID_VIP") == std::string::npos &&
            Name.find("CID_TBD") == std::string::npos;

        if (!bValid)
            continue;

        bool bKnown = false;

        for (auto Existing : CachedSkins)
        {
            if (Existing == CID)
            {
                bKnown = true;
                break;
            }
        }

        if (!bKnown)
            CachedSkins.push_back(CID);
    }

    LoadedSkinScanCursor = End;

    if (LoadedSkinScanCursor >= LoadedSkinScanLimit)
    {
        bLoadedSkinScanCompleted = true;
        LoadedSkinScanCursor = 0;
        LoadedSkinScanLimit = 0;
        AIDebugLogger::Log(
            "Cosmetics",
            "%d resident player skins available for PlayerAI (scan %d)",
            (int)CachedSkins.size(), SkinCacheAttempts);
    }
}

void VersionFeatureAdapter::TickCosmeticCache()
{
    // A few indexed lookups and a small object-array slice per frame keep
    // build-specific skin discovery invisible to TickFlush latency.
    PlayerAITickKnownSkinDiscovery(4);
    PlayerAITickLoadedSkinScan(256);
}

template <typename SoftObjectType>
static bool PlayerAIHasSafeSoftObjectArray(
    const TArray<TSoftObjectPtr<SoftObjectType>>& Array,
    int MaxCount)
{
    const int Count = Array.Num();
    const uint32 Stride = FSoftObjectPtr::Size();

    return Count > 0 && Count <= MaxCount &&
        Array.Max() >= Count &&
        Stride >= 0x10 && Stride <= 0x80 &&
        Array.Data &&
        SDK::MemReadable(
            Array.Data, (size_t)Count * Stride);
}

void VersionFeatureAdapter::ApplyRandomSkin(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    if (!PlayerState)
        return;

    if (CachedSkins.empty())
    {
        ApplyDefaultCosmetics(PlayerState, Pawn);
        return;
    }

    // Some CIDs have unresolvable parts - try a few random picks before
    // falling back to the default outfit.
    for (int Attempt = 0; Attempt < 5; Attempt++)
    {
        auto CID = CachedSkins[rand() % CachedSkins.size()];

        if (!PlayerAIIsLiveSupportObject(CID) ||
            !CID->HasHeroDefinition())
        {
            continue;
        }

        auto Hero = CID->HeroDefinition;
        auto HeroClass = UFortHeroType::StaticClass();

        if (!HeroClass ||
            !PlayerAIIsLiveSupportObject(Hero) ||
            !reinterpret_cast<const UObject*>(Hero)->
                IsA(HeroClass))
            continue;

        // Resolve only already-loaded hero specializations and parts. Loading
        // an unresolved soft path here can stall the entire gameserver.
        const UObject* Parts[6] = {};

        if (Hero->HasSpecializations() &&
            PlayerAIHasSafeSoftObjectArray(
                Hero->Specializations, 64))
        {
            for (int s = 0; s < Hero->Specializations.Num(); s++)
            {
                auto& SpecSoft = Hero->Specializations.Get(s, FSoftObjectPtr::Size());
                auto Spec = const_cast<UFortHeroSpecialization*>(
                    (const UFortHeroSpecialization*)
                    PlayerAITryResolveLoadedSoftObject(
                        SpecSoft, UFortHeroSpecialization::StaticClass()));

                auto SpecObject =
                    reinterpret_cast<const UObject*>(Spec);
                auto SpecClass =
                    UFortHeroSpecialization::StaticClass();

                if (!SpecClass ||
                    !PlayerAIIsLiveSupportObject(SpecObject) ||
                    !SpecObject->IsA(SpecClass) ||
                    !Spec->HasCharacterParts() ||
                    !PlayerAIHasSafeSoftObjectArray(
                        Spec->CharacterParts, 16))
                    continue;

                for (int p = 0; p < Spec->CharacterParts.Num(); p++)
                {
                    auto& PartSoft = Spec->CharacterParts.Get(p, FSoftObjectPtr::Size());
                    auto Part = const_cast<UCustomCharacterPart*>(
                        (const UCustomCharacterPart*)
                        PlayerAITryResolveLoadedSoftObject(
                            PartSoft, UCustomCharacterPart::StaticClass()));

                    auto PartObject =
                        reinterpret_cast<const UObject*>(Part);
                    auto PartClass =
                        UCustomCharacterPart::StaticClass();

                    if (!PartClass ||
                        !PlayerAIIsLiveSupportObject(PartObject) ||
                        !PartObject->IsA(PartClass) ||
                        !Part->HasCharacterPartType())
                        continue;

                    const int Index = Part->CharacterPartType;

                    if (Index >= 0 && Index < 6)
                        Parts[Index] = (const UObject*)Part;
                }
            }
        }

        // Head and body are mandatory. Committing a partially resolved CID
        // while clearing stale slots makes the entire AI invisible on some
        // modern builds.
        if (!PlayerAIIsLiveSupportObject(Parts[0]) ||
            !PlayerAIIsLiveSupportObject(Parts[1]))
            continue;

        if (PlayerAIWriteCharacterParts(PlayerState, Parts))
        {
            PlayerAIWriteHeroType(
                PlayerState, (const UObject*)Hero);

            if (CID->HasGender())
                PlayerAIWriteGender(
                    PlayerState, CID->Gender);

            PlayerAIChoosePartsOnPawn(Pawn, Parts);
            PlayerAIFinishCosmetics(PlayerState, Pawn);
            AIDebugLogger::Verbose("Cosmetics", "applied random skin %s", CID->Name.ToString().c_str());
            return;
        }
    }

    AIDebugLogger::Log("Cosmetics", "random skin resolution failed %d times - using the default outfit (check the 'known skins' count above)", 5);
    ApplyDefaultCosmetics(PlayerState, Pawn);
}

void VersionFeatureAdapter::ResetCaches()
{
    PlayerAILegacyAircraftExitLatchCache = {};
    bEmoteCacheBuilt = false;
    CachedEmoteAssets.clear();
    CachedSkins.clear();
    SkinCacheAttempts = 0;
    bKnownSkinsLoaded = false;
    bLoadedSkinScanCompleted = false;
    KnownSkinScanCursor = 0;
    LoadedSkinScanCursor = 0;
    LoadedSkinScanLimit = 0;
    bCosmeticPathLoadDisabled = false;
    bSoftCosmeticResolveDisabled = false;
    bDefaultCommandoResolved = false;
    PlayerAIDefaultCommando = nullptr;
    bDefaultPartsResolved = false;
    PlayerAIDefaultHead = nullptr;
    PlayerAIDefaultBody = nullptr;
    PlayerAIDefaultBackpack = nullptr;
    PlayerAIPhaseLogicWorld = nullptr;
    PlayerAIPhaseLogic = nullptr;
    PlayerAIPhaseLogicClass = nullptr;
    PlayerAINextPhaseLogicClassResolveTime = 0;
    PlayerAINextPhaseLogicResolveTime = 0;
    PlayerAIPhaseLogicScanCursor = 0;
    PlayerAIPhaseLogicScanLimit = 0;
    PlayerAIResetClassLookup(
        PlayerAIAircraftComponentClassCache);
    PlayerAIResetClassLookup(PlayerAIPawnClassCache);
    PlayerAIResetClassLookup(PlayerAIControllerClassCache);
    PlayerAIResetClassLookup(
        PlayerAIMovementComponentClassCache);
    PlayerAIResetClassLookup(
        PlayerAISafeZoneIndicatorClassCache);
    PlayerAIResetClassLookup(
        PlayerAINetPushModelHelpersClassCache);
    PlayerAIPushModelHelpers = nullptr;
    PlayerAIMarkPropertyDirtyFunction = nullptr;
    PlayerAINextPushModelHelpersResolveTime = 0;
    PlayerAIPlayersLeftDirtyPending = false;
    bGroundTraceDisabled = false;
    GroundTraceCalls = 0;
    GroundTraceHits = 0;
    PlayerAILandingProbeCursor = 0;
    PlayerAILastServerTickTime = -1.f;
    PlayerAIGroundTraceBudgetRemaining =
        PlayerAIGroundTraceBudgetPerTick;
    PlayerAIPhaseScanBudgetAvailable = true;
}
