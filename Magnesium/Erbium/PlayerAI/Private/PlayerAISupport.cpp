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
#include "../Public/VersionFeatureAdapter.h"
#include "../Public/MagnesiumPlayerAISettings.h"
#include "../../Public/Configuration.h"
#include "../../Public/GUI.h"
#include "../../../FortniteGame/Public/FortKismetLibrary.h"
#include "../../../FortniteGame/Public/FortPlayerControllerAthena.h"
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
    if (!bVerbose && !MagnesiumPlayerAISettings::bVerboseLogging)
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

    // Extremely unlikely fallback - still unique.
    std::string Name = "AIPlayer" + std::to_string(rand() % 100000);
    UsedPlayerAINames.insert(Name);
    return Name;
}

void AINameGenerator::Reset()
{
    UsedPlayerAINames.clear();
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
        auto Asset = FindObject<UAthenaDanceItemDefinition>(Path);

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
    auto GameState = GetGameState();

    if (!GameState)
        return nullptr;

    if (GameState->HasAircrafts())
        return GameState->Aircrafts.Num() > 0 ? GameState->Aircrafts[0] : nullptr;

    if (GameState->HasAircraft())
        return GameState->Aircraft;

    return nullptr;
}

bool VersionFeatureAdapter::IsInAircraft(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return false;

    return PC->IsInAircraft();
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

    static auto CompClass = FindClass("FortControllerComponent_Aircraft");

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
    if (!PC || !IsInAircraft(PC))
        return false;

    static auto CompClass = FindClass("FortControllerComponent_Aircraft");

    if (CompClass)
    {
        auto Component = PC->GetAircraftComponent();

        if (Component)
        {
            Component->ServerAttemptAircraftJump(FRotator{});
            return true;
        }
    }

    PC->ServerAttemptAircraftJump(FRotator{});
    return true;
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

FVector VersionFeatureAdapter::FindGroundLocation(const FVector& Near, bool& bOutFound, AFortPlayerPawnAthena* IgnorePawn)
{
    bOutFound = false;

    static bool bGroundTraceDisabled = false;

    if (bGroundTraceDisabled)
        return Near;

    auto World = UWorld::GetWorld();

    if (!World)
        return Near;

    FVector Ground{};

    if (!PlayerAITryGroundTrace(World, IgnorePawn, Near, Ground))
    {
        bGroundTraceDisabled = true;
        AIDebugLogger::MissingFeature("GroundTraceForPlayerAI",
            "native ground trace faulted and was disabled - PlayerAI uses flat movement fallback");
        return Near;
    }

    if (Ground.X == 0.f && Ground.Y == 0.f && Ground.Z == 0.f)
        return Near;

    bOutFound = true;
    return Ground;
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

bool VersionFeatureAdapter::TryGetSafeZone(FVector& OutCenter, float& OutRadius)
{
    auto GameMode = GetGameMode();

    if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
        return false;

    auto Indicator = GameMode->SafeZoneIndicator;

    if (Indicator->HasNextCenter() && Indicator->HasNextRadius())
    {
        OutCenter = Indicator->NextCenter;
        OutRadius = Indicator->NextRadius;
        return OutRadius > 0.f;
    }

    // Old builds.
    if (Indicator->HasLastCenter() && Indicator->HasRadius())
    {
        OutCenter = Indicator->LastCenter;
        OutRadius = Indicator->Radius;
        return OutRadius > 0.f;
    }

    return false;
}

bool VersionFeatureAdapter::IsInsideSafeZone(const FVector& Location)
{
    auto GameMode = GetGameMode();

    if (!GameMode || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator)
        return true; // no storm yet - everywhere is safe

    static int HasNativeCheck = -1;

    if (HasNativeCheck == -1)
        HasNativeCheck = GameMode->GetFunction("IsInCurrentSafeZone") != nullptr ? 1 : 0;

    if (HasNativeCheck == 1)
        return GameMode->IsInCurrentSafeZone(Location, false);

    // Fallback: distance check against the target circle.
    FVector Center{};
    float Radius = 0.f;

    if (!TryGetSafeZone(Center, Radius))
        return true;

    FVector Flat = Location;
    Flat.Z = Center.Z;
    return Flat.GetDistanceTo(Center) <= Radius;
}

float VersionFeatureAdapter::GetStormDamagePerSecond()
{
    auto GameMode = GetGameMode();

    int Phase = 1;

    if (GameMode)
    {
        if (GameMode->HasSafeZonePhase() && GameMode->SafeZonePhase > 0)
            Phase = GameMode->SafeZonePhase;
        else if (GameMode->HasSafeZoneIndicator() && GameMode->SafeZoneIndicator && GameMode->SafeZoneIndicator->HasCurrentPhase())
            Phase = GameMode->SafeZoneIndicator->CurrentPhase;
    }

    // Version specific damage info when available.
    if (GameMode && GameMode->HasSafeZoneIndicator() && GameMode->SafeZoneIndicator &&
        GameMode->SafeZoneIndicator->HasCurrentDamageInfo() && FFortSafeZoneDamageInfo::HasDamage())
    {
        float Damage = GameMode->SafeZoneIndicator->CurrentDamageInfo.Damage;

        if (std::isfinite((double)Damage) && Damage > 0.f)
        {
            if (FFortSafeZoneDamageInfo::HasbPercentageBasedDamage() && GameMode->SafeZoneIndicator->CurrentDamageInfo.bPercentageBasedDamage)
                Damage *= 100.f;

            return Damage < 1.f ? 1.f : Damage;
        }
    }

    // Generic per-phase defaults.
    static const float PhaseDamage[] = { 1.f, 1.f, 1.f, 2.f, 5.f, 7.f, 8.f, 10.f, 10.f, 10.f };
    const int Index = Phase < 0 ? 0 : (Phase > 9 ? 9 : Phase);
    return PhaseDamage[Index];
}

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

void VersionFeatureAdapter::ApplyDefaultCosmetics(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    if (!PlayerState)
        return;

    // Default hero + character parts exist on every supported version; skins
    // beyond this are version specific and intentionally skipped
    // ("cosmetics only if supported").
    static auto Commando = FindObject(L"/Game/Athena/Heroes/HID_001_Athena_Commando_F.HID_001_Athena_Commando_F", nullptr);
    static auto Commando2 = FindObject(L"/Game/Athena/Heroes/HID_Commando_Athena_01.HID_Commando_Athena_01", nullptr);

    if (PlayerState->HasHeroType())
        PlayerState->HeroType = Commando ? Commando : Commando2;

    static auto Head = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");
    static auto Body = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01");
    static auto Backpack = FindObject<UObject>(L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack");

    if (!Head || !Body)
    {
        AIDebugLogger::MissingFeature("DefaultCharacterParts", "PlayerAI keeps engine default appearance");
        return;
    }

    // Mirrors the character part layout handling real players get - struct
    // based CharacterParts on newer versions, flat array on older ones.
    static auto NewStyleCharacterPartsOffset = PlayerState->GetOffset("CharacterParts", 0x100000);

    if (NewStyleCharacterPartsOffset == -1)
    {
        static auto CharacterPartsOff = [&]
            {
                auto Off = PlayerState->GetOffset("CharacterParts");
                if (Off == -1)
                    Off = PlayerState->GetOffset("LocalCharacterParts");
                return Off;
            }();

        if (CharacterPartsOff == -1)
        {
            AIDebugLogger::MissingFeature("CharacterParts", "PlayerAI keeps engine default appearance");
            return;
        }

        auto& CharacterParts = GetFromOffset<const UObject* [0x6]>(PlayerState, CharacterPartsOff);
        CharacterParts[0] = Head;
        CharacterParts[1] = Body;
        CharacterParts[3] = Backpack;
    }
    else
    {
        static auto PartsStruct = FindStruct("CustomCharacterParts");

        if (!PartsStruct)
        {
            AIDebugLogger::MissingFeature("CustomCharacterParts", "PlayerAI keeps engine default appearance");
            return;
        }

        static auto PartsOffset = PartsStruct->GetOffset("Parts");
        static auto CharacterPartsOff = PlayerState->GetOffset("CharacterParts");

        if (PartsOffset == -1 || CharacterPartsOff == -1)
        {
            AIDebugLogger::MissingFeature("CustomCharacterParts.Parts", "PlayerAI keeps engine default appearance");
            return;
        }

        auto CharacterPartsPtr = (PBYTE)PlayerState + CharacterPartsOff;
        auto& CharacterParts = GetFromOffset<const UObject* [0x6]>(CharacterPartsPtr, PartsOffset);
        CharacterParts[0] = Head;
        CharacterParts[1] = Body;
        CharacterParts[3] = Backpack;
    }

    // Native customization is optional polish on top of the replicated
    // character parts; when it faults for server-side players it gets
    // disabled instead of crashing the gameserver.
    static bool bNativeCustomizationDisabled = false;

    if (ApplyCharacterCustomization && Pawn && !bNativeCustomizationDisabled)
    {
        if (!PlayerAITryNativeCustomization(ApplyCharacterCustomization, PlayerState, Pawn))
        {
            bNativeCustomizationDisabled = true;
            AIDebugLogger::MissingFeature("NativeCharacterCustomizationForPlayerAI",
                "native customization call faulted and was disabled - parts replicate via the player state");
        }
    }
}

void VersionFeatureAdapter::ResetCaches()
{
    bEmoteCacheBuilt = false;
    CachedEmoteAssets.clear();
}
