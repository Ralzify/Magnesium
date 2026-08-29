#pragma once
#include "SupportTypes.h"
#include "../../../FortniteGame/Public/FortGameMode.h"

enum class EPlayerAIAircraftDropState : uint8_t
{
    Unknown, Locked, Open,
};

enum class EPlayerAICosmeticLoadPolicy : uint8_t
{
    ResidentOnly,

    AllowSynchronousLoad,
};

class VersionFeatureAdapter
{
public:
    static AFortGameMode* GetGameMode();
    static AFortGameStateAthena* GetGameState();
    static float GetTimeSeconds();

    static bool IsLiveObject(const UObject* Object);
    static bool IsLiveActor(const AActor* Actor);

    using FIsManagedAIControllerFn = bool (*)(const AFortPlayerControllerAthena*);
    using FHasManagedAIControllersFn = bool (*)();
    static void SetManagedAIControllerHooks(FIsManagedAIControllerFn IsManaged,
        FHasManagedAIControllersFn HasAny);
    static bool IsManagedAIController(const AFortPlayerControllerAthena* PC);
    static bool HasManagedAIControllers();

    static void BeginServerTick(float TimeSeconds);

    static void TickServerFrame(const UNetDriver* Driver);

    static void MarkSyntheticParticipantReady(AFortPlayerControllerAthena* PC,
        AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn);

    static void InitializeSyntheticPawnCosmeticLifecycle(AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn);

    // Calls a UFunction with a correctly sized, zeroed parameter buffer inside a fault guard.
    static bool SafeCallNoArgs(const UObject* Obj, UFunction* Fn);

    static int GetMaxPlayerCount();

    static EPlayerAIMatchPhase GetMatchPhase();

    // Replication is explicitly dirtied for push-model builds (Chapter 4+).
    static int CountAliveParticipants();
    static bool MarkReplicatedPropertyDirty(const UObject* Object, const wchar_t* PropertyName);
    static void ReplicatePlayersLeft(AFortGameStateAthena* GameState, int PlayersLeft, bool bForce = false);
    static void SyncPlayersLeft(bool bForce = false);
    static void RetryPendingPlayersLeftReplication();

    static bool SupportsEmotes();
    static UObject* GetRandomEmoteAsset();
    static void PlayEmote(AFortPlayerControllerAthena* PC, UObject* EmoteAsset);

    static bool SupportsThankDriver(AFortPlayerControllerAthena* PC);
    static bool ThankDriver(AFortPlayerControllerAthena* PC);
    static AFortAthenaAircraft* GetAircraft();
    static EPlayerAIAircraftDropState GetAircraftDropState(float TimeSeconds);
    static bool IsInAircraft(AFortPlayerControllerAthena* PC);
    static bool EnterAircraft(AFortPlayerControllerAthena* PC);
    static bool JumpFromAircraft(AFortPlayerControllerAthena* PC);
    static bool TryBeginSkydiving(AFortPlayerPawnAthena* Pawn);
    static void ForceLeaveAircraft(AFortPlayerControllerAthena* PC);
    static bool MarkVirtualAircraftExited(AFortPlayerControllerAthena* PC);

    static FVector FindGroundLocation(const FVector& Near, bool& bOutFound,
        AFortPlayerPawnAthena* IgnorePawn = nullptr);
    static bool TryResolveGroundedLandingSpot(const FVector& Desired,
        AFortPlayerPawnAthena* IgnorePawn, FVector& OutSpot);
    static bool TryIsPawnGrounded(AFortPlayerPawnAthena* Pawn, bool& OutGrounded);

    static bool IsGroundTraceReliable();
    static bool SupportsCrouch(AFortPlayerPawnAthena* Pawn);
    static bool SupportsGliding();

    static bool TryGetSafeZone(FVector& OutCenter, float& OutRadius);
    static bool IsInsideSafeZone(const FVector& Location);
    static bool IsStormClosed(float TimeSeconds);
    static float GetStormDamagePerSecond();

    static bool SupportsDBNO();

    static UAthenaCharacterItemDefinition* ResolveCharacterSkin(const char* IdOrPath,
        EPlayerAICosmeticLoadPolicy LoadPolicy = EPlayerAICosmeticLoadPolicy::ResidentOnly);

    static bool ApplyCharacterSkin(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn,
        UAthenaCharacterItemDefinition* Character, EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly, bool* OutTargetRetained = nullptr);

    static UAthenaCharacterItemDefinition* ApplyRandomSkin(AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn, EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly);

    static bool QueueRandomSkin(AFortPlayerControllerAthena* PC,
        AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn,
        UAthenaCharacterItemDefinition** AppliedImmediately = nullptr);

    static bool QueueRequestedSkin(AFortPlayerControllerAthena* PC,
        AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn,
        UAthenaCharacterItemDefinition* Character);

    static void TickCosmeticCache();

    static bool IsSkinCommitPending(AFortPlayerPawnAthena* Pawn);

    static uint64 GetSkinCommitProgressGeneration();

    static bool IsSkinCommitActivelyWorking(AFortPlayerPawnAthena* Pawn);

    static void CancelSkinCommit(AFortPlayerPawnAthena* Pawn);

    static bool KillPawn(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* KillerPC,
        AActor* DamageCauser);

    static bool ApplyDefaultCosmetics(AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn, EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly);

    static bool ApplyFixedDefaultCosmetics(AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn, EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly);

    static void ResetCaches();
};
