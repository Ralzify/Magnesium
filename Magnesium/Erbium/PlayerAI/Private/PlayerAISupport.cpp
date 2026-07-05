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
#include "../../Public/Configuration.h"
#include "../../Public/GUI.h"
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
    if (!PC)
        return false;

    // Actually flagged aboard (should not happen anymore - the EnterAircraft
    // hook skips PlayerAI - but old saves/edge versions): try the native
    // jump RPC first, some versions accept server-side calls.
    if (IsInAircraft(PC))
    {
        static auto CompClass = FindClass("FortControllerComponent_Aircraft");

        const UObject* Target = PC;

        if (CompClass)
        {
            auto Component = PC->GetAircraftComponent();

            if (Component)
                Target = Component;
        }

        auto Fn = Target->GetFunction("ServerAttemptAircraftJump");

        if (Fn)
            SafeCallNoArgs(Target, Fn);

        if (!IsInAircraft(PC) && PC->MyFortPawn)
            return true;
    }

    // The RPC rejected the jump (its native validation ignores
    // connectionless controllers on pre-C2 versions) or the AI was never
    // aboard: run the same sequence Magnesium's own ServerAttemptAircraftJump
    // hook uses for modern versions and lategame - a fresh pawn through
    // RestartPlayer, off the aircraft's books.
    auto GameMode = GetGameMode();

    if (!GameMode)
        return false;

    PC->StateName = FName(L"Inactive");

    if (PC->Pawn)
        PC->UnPossess(PC->Pawn);

    GameMode->RestartPlayer(PC);
    PC->SetControlRotation(FRotator{});

    auto Pawn = PC->MyFortPawn;

    AIDebugLogger::Verbose("Transport", "restart jump: pawn %d, still aboard: %d",
        Pawn ? 1 : 0, IsInAircraft(PC) ? 1 : 0);

    return Pawn != nullptr;
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

    if (!Mask)
        return false;

    GetFromOffset<uint8>(Obj, Offset) &= ~Mask;
    return true;
}

void VersionFeatureAdapter::ForceLeaveAircraft(AFortPlayerControllerAthena* PC)
{
    if (!PC)
        return;

    bool bCleared = PlayerAITryClearInAircraftBit(PC);

    if (PC->PlayerState)
        bCleared |= PlayerAITryClearInAircraftBit(PC->PlayerState);

    AIDebugLogger::Verbose("Transport", "force leave aircraft: flag %s, still aboard: %d",
        bCleared ? "cleared" : "not found", IsInAircraft(PC) ? 1 : 0);
}

// Starts the native skydive (BeginSkydiving(bFromAircraft=true)) through a
// sized parameter buffer - the engine then owns descent, glider deploy and
// landing exactly like a real bus jumper.
bool VersionFeatureAdapter::TryBeginSkydiving(AFortPlayerPawnAthena* Pawn)
{
    if (!Pawn)
        return false;

    auto Fn = Pawn->GetFunction("BeginSkydiving");

    if (!Fn)
    {
        AIDebugLogger::MissingFeature("BeginSkydivingForPlayerAI",
            "no BeginSkydiving on this version - PlayerAI lands by direct placement");
        return false;
    }

    int Size = 0;
    int BoolOffset = -1;
    {
        auto Params = Fn->GetParams();
        Size = Params.Size;

        for (auto& Param : Params.NameOffsetMap)
        {
            // First byte-sized input parameter = bFromAircraft.
            if ((Param.PropertyFlags & 0x400) == 0 && Param.ElementSize == 1)
            {
                BoolOffset = (int)Param.Offset;
                break;
            }
        }
    }

    if (Size < 0 || Size > 0x1000)
        Size = 0x1000;

    std::vector<uint8_t> Buffer((size_t)(Size > 0 ? Size : 1), 0);

    if (BoolOffset >= 0 && BoolOffset < Size)
        Buffer[(size_t)BoolOffset] = 1; // bFromAircraft = true

    return PlayerAIGuardedProcessEvent(Pawn, Fn, Buffer.data());
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

bool VersionFeatureAdapter::IsGroundTraceReliable()
{
    if (bGroundTraceDisabled)
        return false;

    // Needs a real success rate, not just "did not crash": some versions
    // return zero vectors from the trace most of the time.
    if (GroundTraceCalls >= 20 && GroundTraceHits * 10 < GroundTraceCalls * 3)
    {
        bGroundTraceDisabled = true;
        AIDebugLogger::MissingFeature("GroundTraceForPlayerAI",
            "ground trace rarely finds terrain on this version - PlayerAI runs trace-free");
        return false;
    }

    return true;
}

FVector VersionFeatureAdapter::FindGroundLocation(const FVector& Near, bool& bOutFound, AFortPlayerPawnAthena* IgnorePawn)
{
    bOutFound = false;

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
            "native ground trace faulted and was disabled - PlayerAI runs trace-free");
        return Near;
    }

    GroundTraceCalls++;

    if (Ground.X == 0.f && Ground.Y == 0.f && Ground.Z == 0.f)
        return Near;

    GroundTraceHits++;
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

    // Version specific damage info when available. Percentage semantics vary
    // between versions, so the result is clamped to a sane per-second range -
    // the fallback must never insta-melt a full-health PlayerAI.
    if (GameMode && GameMode->HasSafeZoneIndicator() && GameMode->SafeZoneIndicator &&
        GameMode->SafeZoneIndicator->HasCurrentDamageInfo() && FFortSafeZoneDamageInfo::HasDamage())
    {
        float Damage = GameMode->SafeZoneIndicator->CurrentDamageInfo.Damage;

        if (std::isfinite((double)Damage) && Damage > 0.f)
        {
            if (FFortSafeZoneDamageInfo::HasbPercentageBasedDamage() && GameMode->SafeZoneIndicator->CurrentDamageInfo.bPercentageBasedDamage)
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

// ---- Weapon fire ---------------------------------------------------------------------

// (Kept free of unwindable C++ objects so SEH is allowed here.)
static bool PlayerAITryActivateAbility(UAbilitySystemComponent* ASC, FGameplayAbilitySpecHandle Handle)
{
    GPlayerAIGuardedNativeCallDepth++;
    bool bOk;

    __try
    {
        // Same server-side activation path Magnesium routes real client
        // ability activation through.
        uint8_t PredictionKey[0x40] = {};
        UAbilitySystemComponent::InternalServerTryActivateAbility(ASC, Handle, true, (FPredictionKey*)PredictionKey, nullptr);
        bOk = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bOk = false;
    }

    GPlayerAIGuardedNativeCallDepth--;
    return bOk;
}

bool VersionFeatureAdapter::TryActivateAbilityHandle(UAbilitySystemComponent* ASC, FGameplayAbilitySpecHandle Handle)
{
    if (!ASC)
        return false;

    return PlayerAITryActivateAbility(ASC, Handle);
}

bool VersionFeatureAdapter::TryFireEquippedWeapon(AFortPlayerControllerAthena* PC, AFortPlayerPawnAthena* Pawn)
{
    static bool bWeaponFireDisabled = false;
    static int WeaponFireFaults = 0;

    if (bWeaponFireDisabled || !PC || !Pawn)
        return false;

    auto PlayerState = (AFortPlayerStateAthena*)PC->PlayerState;

    if (!PlayerState || !PlayerState->HasAbilitySystemComponent() || !PlayerState->AbilitySystemComponent)
        return false;

    auto Weapon = Pawn->HasCurrentWeapon() ? (AFortWeapon*)Pawn->CurrentWeapon : nullptr;

    if (!Weapon)
        return false;

    if (!Weapon->HasPrimaryAbilitySpecHandle())
    {
        bWeaponFireDisabled = true;
        AIDebugLogger::MissingFeature("WeaponFireAbilityForPlayerAI",
            "no primary ability handle on this version - shots stay simulated (no gunfire cosmetics)");
        return false;
    }

    FGameplayAbilitySpecHandle Handle = Weapon->PrimaryAbilitySpecHandle;

    if (Handle.Handle == 0 || Handle.Handle == -1)
        return false; // weapon not fully initialized yet

    auto ASC = PlayerState->AbilitySystemComponent;

    if (!PlayerAITryActivateAbility(ASC, Handle))
    {
        if (++WeaponFireFaults >= 3)
        {
            bWeaponFireDisabled = true;
            AIDebugLogger::MissingFeature("WeaponFireAbilityForPlayerAI",
                "fire ability activation faulted and was disabled - shots stay simulated");
        }
        return false;
    }

    // Release the simulated trigger right away so automatic weapons fire a
    // single burst per simulated shot instead of looping forever.
    auto Spec = ASC->ActivatableAbilities.Items.Search(
        [&](FGameplayAbilitySpec& Item)
        {
            return Item.Handle.Handle == Handle.Handle;
        },
        FGameplayAbilitySpec::Size());

    if (Spec)
    {
        Spec->InputPressed = false;
        ASC->ActivatableAbilities.MarkItemDirty(*Spec);
    }

    return true;
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

// Writes character part pointers (indexed by EFortCustomPartType, non-null
// slots only) into the player state, handling both the struct based
// CharacterParts layout of newer versions and the flat array of older ones.
static bool PlayerAIWriteCharacterParts(AFortPlayerStateAthena* PlayerState, const UObject* Parts[6])
{
    static auto NewStyleCharacterPartsOffset = PlayerState->GetOffset("CharacterParts", 0x100000);

    const UObject** Target = nullptr;

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
            return false;
        }

        Target = GetFromOffset<const UObject* [0x6]>(PlayerState, CharacterPartsOff);
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
    {
        if (Parts[i])
            Target[i] = Parts[i];
    }

    return true;
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
    auto OnRepCharacterData = PlayerState->GetFunction("OnRep_CharacterData");

    if (OnRepCharacterData)
        PlayerState->ProcessEvent(OnRepCharacterData, nullptr);

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

void VersionFeatureAdapter::ApplyDefaultCosmetics(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    if (!PlayerState)
        return;

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

    const UObject* Parts[6] = { Head, Body, nullptr, Backpack, nullptr, nullptr };

    if (PlayerAIWriteCharacterParts(PlayerState, Parts))
    {
        PlayerAIChoosePartsOnPawn(Pawn, Parts);
        PlayerAIFinishCosmetics(PlayerState, Pawn);
    }
}

// ---- Random skins from the hosted build ---------------------------------------------

static int SkinCacheAttempts = 0;
static std::vector<UAthenaCharacterItemDefinition*> CachedSkins;
static bool bCosmeticPathLoadDisabled = false;

// Fault-isolated cosmetic asset loading: a faulting load must never take a
// PlayerAI spawn down. One fault disables path loading for the session
// (already-loaded parts keep resolving through the soft pointers).
// (Kept free of unwindable C++ objects for SEH.)
static const UObject* PlayerAITryLoadCosmetic(const wchar_t* Path, const UClass* Class)
{
    GPlayerAIGuardedNativeCallDepth++;

    const UObject* Result = nullptr;
    bool bFaulted = false;

    __try
    {
        Result = FindObject(Path, Class);
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
            "cosmetic asset loading faulted and was disabled - only pre-loaded skins are used");
    }

    return Result;
}

static const UObject* PlayerAILoadCosmeticByPath(const UEAllocatedString& Path, const UClass* Class)
{
    if (bCosmeticPathLoadDisabled || Path.empty() || Path == "None" || !Class)
        return nullptr;

    UEAllocatedWString Wide(Path.begin(), Path.end());
    return PlayerAITryLoadCosmetic(Wide.c_str(), Class);
}

// Known character definitions with stable names since the early seasons.
// Cosmetics are cumulative in the paks, so these exist on essentially every
// build; each is force-loaded through the fault-guarded loader and simply
// skipped when a build lacks it. This guarantees skin variety even though
// a fresh server has almost no cosmetics loaded in memory.
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

static bool bKnownSkinsLoaded = false;

static void PlayerAILoadKnownSkins()
{
    if (bKnownSkinsLoaded)
        return;

    bKnownSkinsLoaded = true;

    auto AddSkin = [&](UAthenaCharacterItemDefinition* CID)
        {
            if (!CID || !CID->HasHeroDefinition() || !CID->HeroDefinition)
                return;

            for (auto Existing : CachedSkins)
                if (Existing == CID)
                    return;

            CachedSkins.push_back(CID);
        };

    for (auto Name : PlayerAIKnownSkinNames)
    {
        if (bCosmeticPathLoadDisabled)
            break;

        UEAllocatedWString Path = UEAllocatedWString(L"/Game/Athena/Items/Cosmetics/Characters/") + Name + L"." + Name;
        auto CID = (UAthenaCharacterItemDefinition*)PlayerAITryLoadCosmetic(Path.c_str(), UAthenaCharacterItemDefinition::StaticClass());
        AddSkin(CID);
    }

    AIDebugLogger::Log("Cosmetics", "%d known skins force-loaded for PlayerAI", (int)CachedSkins.size());
}

static void PlayerAIBuildSkinCache()
{
    // Guaranteed baseline: force-load the known skin set once.
    PlayerAILoadKnownSkins();

    // Then keep merging in whatever else is loaded in this build's memory
    // (organic variety) until a reasonable set exists.
    if (CachedSkins.size() >= 24 || SkinCacheAttempts >= 30)
        return;

    SkinCacheAttempts++;

    for (int i = 0; i < TUObjectArray::Num(); i++)
    {
        auto Object = TUObjectArray::GetObjectByIndex(i);

        if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA<UAthenaCharacterItemDefinition>())
            continue;

        auto CID = (UAthenaCharacterItemDefinition*)Object;

        if (!CID->HasHeroDefinition() || !CID->HeroDefinition)
            continue;

        auto RawName = CID->Name.ToString();
        std::string Name(RawName.c_str());

        // Player skins only: no NPC/VIP/placeholder cosmetics.
        const bool bValid = (Name.find("Athena_Commando") != std::string::npos || Name.find("CID_Character") != std::string::npos) &&
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

    AIDebugLogger::Log("Cosmetics", "%d player skins available for PlayerAI (scan %d)",
        (int)CachedSkins.size(), SkinCacheAttempts);
}

void VersionFeatureAdapter::ApplyRandomSkin(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn)
{
    if (!PlayerState)
        return;

    PlayerAIBuildSkinCache();

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
        auto Hero = CID ? CID->HeroDefinition : nullptr;

        if (!Hero)
            continue;

        // Resolve the hero specialization character parts (soft references -
        // resolved by pointer first, force-loaded by path as fallback).
        const UObject* Parts[6] = {};
        bool bAnyPart = false;

        if (Hero->HasSpecializations())
        {
            for (int s = 0; s < Hero->Specializations.Num(); s++)
            {
                auto& SpecSoft = Hero->Specializations.Get(s, FSoftObjectPtr::Size());
                auto Spec = const_cast<UFortHeroSpecialization*>((const UFortHeroSpecialization*)SpecSoft.Get());

                if (!Spec)
                {
                    auto SpecPath = SpecSoft.ObjectID.AssetPathName.ToString();
                    Spec = (UFortHeroSpecialization*)PlayerAILoadCosmeticByPath(SpecPath, UFortHeroSpecialization::StaticClass());
                }

                if (!Spec || !Spec->HasCharacterParts())
                    continue;

                for (int p = 0; p < Spec->CharacterParts.Num(); p++)
                {
                    auto& PartSoft = Spec->CharacterParts.Get(p, FSoftObjectPtr::Size());
                    auto Part = const_cast<UCustomCharacterPart*>((const UCustomCharacterPart*)PartSoft.Get());

                    if (!Part)
                    {
                        auto PartPath = PartSoft.ObjectID.AssetPathName.ToString();
                        Part = (UCustomCharacterPart*)PlayerAILoadCosmeticByPath(PartPath, UCustomCharacterPart::StaticClass());
                    }

                    if (!Part || !Part->HasCharacterPartType())
                        continue;

                    const int Index = Part->CharacterPartType;

                    if (Index >= 0 && Index < 6)
                    {
                        Parts[Index] = (const UObject*)Part;
                        bAnyPart = true;
                    }
                }
            }
        }

        if (!bAnyPart)
            continue;

        if (PlayerState->HasHeroType())
            PlayerState->HeroType = (const UObject*)Hero;

        if (PlayerAIWriteCharacterParts(PlayerState, Parts))
        {
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
    bEmoteCacheBuilt = false;
    CachedEmoteAssets.clear();
}
