#pragma once
// ============================================================================
// Magnesium PlayerAI - VersionFeatureAdapter
//
// Central abstraction layer for every version specific feature the PlayerAI
// system touches. Every feature is checked for existence before use; when a
// feature is missing the adapter reports "unsupported" and the behavior
// modules fall back to plain movement / skipping the feature. Nothing in
// here modifies native AI systems (henchmen, guards, bosses, wildlife) -
// it only *reads* engine state and calls player-level functions that real
// players use as well.
// ============================================================================
#include "PlayerAITypes.h"
#include "../../../FortniteGame/Public/FortGameMode.h"

enum class EPlayerAIAircraftDropState : uint8_t
{
    Unknown,
    Locked,
    Open,
};

enum class EPlayerAICosmeticLoadPolicy : uint8_t
{
    // PlayerAI's background spawn path must never pause the server to load a
    // cosmetic package.
    ResidentOnly,

    // Explicit cheat-bot spawning is user initiated, so it may load the one
    // selected outfit and its parts synchronously.
    AllowSynchronousLoad,
};

class VersionFeatureAdapter
{
public:
    // ---- Core lookups (all null-checked) -----------------------------------
    static AFortGameMode* GetGameMode();
    static AFortGameStateAthena* GetGameState();
    static float GetTimeSeconds();

    // Opens one bounded PlayerAI work budget for the current server frame.
    // Expensive native probes consume this shared budget instead of every AI
    // issuing the same work in one TickFlush.
    static void BeginServerTick(float TimeSeconds);

    // Server-owned controllers never send the client loading/possession RPCs
    // that normally unlock character customization. Publish the equivalent
    // reflected readiness state for both PlayerAI and cheat-spawned bots.
    static void MarkSyntheticParticipantReady(
        AFortPlayerControllerAthena* PC,
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn);

    // Connectionless FN19-FN31 pawns can be possessed without receiving the
    // PlayerState repnotify that creates/binds their cosmetic component. Run
    // that lifecycle once after readiness and before any outfit or equipment.
    static void InitializeSyntheticPawnCosmeticLifecycle(
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn);

    // ---- Guarded native invocation ------------------------------------------
    // Calls a UFunction with a correctly sized, zeroed parameter buffer
    // inside a fault guard (never passes null parms to natives that expect
    // arguments). Returns false when the function is missing or the native
    // call faulted - contained, never crashing the gameserver.
    static bool SafeCallNoArgs(const UObject* Obj, UFunction* Fn);

    // Max player count of the running gameserver: GameSession->MaxPlayers,
    // falling back to the playlist MaxPlayers, falling back to 100.
    static int GetMaxPlayerCount();

    // Version independent match phase (derived from gsStatus + GamePhase).
    static EPlayerAIMatchPhase GetMatchPhase();

    // Authoritative alive-controller count shared by the legacy
    // AlivePlayers list and the native-bot AliveBots list. Replication is
    // explicitly dirtied for push-model builds (Chapter 4+).
    static int CountAliveParticipants();
    static bool MarkReplicatedPropertyDirty(const UObject* Object, const wchar_t* PropertyName);
    static void ReplicatePlayersLeft(AFortGameStateAthena* GameState, int PlayersLeft, bool bForce = false);
    static void SyncPlayersLeft(bool bForce = false);
    static void RetryPendingPlayersLeftReplication();

    // ---- Emotes -------------------------------------------------------------
    static bool SupportsEmotes();
    // Random emote asset that exists on this version, nullptr if unsupported.
    static UObject* GetRandomEmoteAsset();
    static void PlayEmote(AFortPlayerControllerAthena* PC, UObject* EmoteAsset);

    // ---- Transport / bus phase ----------------------------------------------
    static bool SupportsThankDriver(AFortPlayerControllerAthena* PC);
    static bool ThankDriver(AFortPlayerControllerAthena* PC);
    static AFortAthenaAircraft* GetAircraft();
    // Tri-state authoritative drop-window query. Locked signals always win;
    // callers must not treat unavailable/contradictory metadata as unlocked.
    static EPlayerAIAircraftDropState GetAircraftDropState(float TimeSeconds);
    static bool IsInAircraft(AFortPlayerControllerAthena* PC);
    // Puts an AI player into the aircraft using the same native path real
    // players use. Returns false when unavailable (caller uses fallback).
    static bool EnterAircraft(AFortPlayerControllerAthena* PC);
    // Takes an AI player off the aircraft and gives it a fresh pawn (native
    // jump RPC when the version accepts it, otherwise the same RestartPlayer
    // sequence Magnesium's own jump hook uses). True when a pawn exists.
    static bool JumpFromAircraft(AFortPlayerControllerAthena* PC);
    // Starts the native skydive on a pawn (fault guarded, version safe).
    static bool TryBeginSkydiving(AFortPlayerPawnAthena* Pawn);
    // Last-resort unboard: clears the replicated in-aircraft flag directly
    // (controller and player state) when the native jump refuses to.
    static void ForceLeaveAircraft(AFortPlayerControllerAthena* PC);
    // Completes the private legacy controller exit transition used by the
    // route-end failed-loader check. This is a cheap PlayerAI-only operation;
    // false means unsupported or that the per-build layout failed validation.
    static bool MarkVirtualAircraftExited(AFortPlayerControllerAthena* PC);

    // ---- Movement / world ----------------------------------------------------
    // Ground location under/near a world position (safe fallback: input).
    // Pass the asking pawn when available so the trace ignores it; if the
    // native trace faults on a version it disables itself for the session.
    static FVector FindGroundLocation(const FVector& Near, bool& bOutFound, AFortPlayerPawnAthena* IgnorePawn = nullptr);
    // Resolves an actual traced landing point at the requested XY or a
    // deterministic nearby spiral. Returns false instead of inventing a Z.
    static bool TryResolveGroundedLandingSpot(const FVector& Desired, AFortPlayerPawnAthena* IgnorePawn, FVector& OutSpot);
    // Queries CharacterMovement::IsMovingOnGround (or its explicit walking
    // movement mode) through guarded reflection.
    // The return value reports whether the query was supported; OutGrounded
    // is meaningful only when this returns true.
    static bool TryIsPawnGrounded(AFortPlayerPawnAthena* Pawn, bool& OutGrounded);

    // True while ground tracing works on this version. When false, all
    // movement/landing logic runs trace-free (native physics + landing
    // target anchors) instead of treating "no ground data" as blocked.
    static bool IsGroundTraceReliable();
    static bool SupportsCrouch(AFortPlayerPawnAthena* Pawn);
    static bool SupportsGliding();

    // ---- Safe zone / storm -----------------------------------------------------
    // Current safe zone target circle. Returns false while no storm exists.
    static bool TryGetSafeZone(FVector& OutCenter, float& OutRadius);
    static bool IsInsideSafeZone(const FVector& Location);
    static bool IsStormClosed(float TimeSeconds);
    static float GetStormDamagePerSecond();

    // ---- DBNO -------------------------------------------------------------------
    static bool SupportsDBNO();

    // ---- Cosmetics -------------------------------------------------------------------
    // Picks a random character skin from the cosmetics that exist in the
    // hosted build and applies it (hero + character parts). Falls back to
    // the default soldier when the build has no usable skins.
    // Returns the exact CID that was successfully applied. Callers retain it
    // for features such as skin-specific map icons on builds whose synthetic
    // PlayerState has no reflected HeroType field.
    static UAthenaCharacterItemDefinition* ResolveCharacterSkin(
        const char* IdOrPath,
        EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly);

    // Applies one already-resolved CID. No partial outfit is committed: head
    // and body must both resolve, otherwise callers can safely use default.
    static bool ApplyCharacterSkin(
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn,
        UAthenaCharacterItemDefinition* Character,
        EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly,
        bool* OutTargetRetained = nullptr);

    static UAthenaCharacterItemDefinition* ApplyRandomSkin(
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn,
        EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly);

    // Chooses current-build CIDs without replacement and resolves one soft
    // reference at a time through bounded asynchronous requests. The queue is
    // game-thread-only and world-bound; it never performs synchronous package
    // IO, and unavailable entries fall back safely.
    static bool QueueRandomSkin(
        AFortPlayerControllerAthena* PC,
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn,
        UAthenaCharacterItemDefinition** AppliedImmediately = nullptr);

    // Defers a user-requested CID (or a safe default when Character is null)
    // so counted cheat-bot commands never rebuild every pawn in one frame.
    static bool QueueRequestedSkin(
        AFortPlayerControllerAthena* PC,
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn,
        UAthenaCharacterItemDefinition* Character);

    // Amortizes discovery and random-bot commits without blocking package IO or
    // walking the full UObject array in one frame. Explicit CID/default requests
    // may perform one user-authorized synchronous resolution, but all resulting
    // pawn mesh commits share the one-per-tick budget; explicit resolution is
    // retried in small cached batches until the complete outfit is resident.
    static void TickCosmeticCache();

    // True while the bounded cheat-bot cosmetic queue still owns this pawn.
    // Spawn equipment waits for this to clear so mesh reconstruction cannot
    // race a newly created weapon actor.
    static bool IsSkinCommitPending(
        AFortPlayerPawnAthena* Pawn);

    // Monotonic heartbeat advanced only when the guarded bot-cosmetic queue
    // starts or completes meaningful work. Equipment uses it to distinguish
    // a healthy serialized batch from a genuinely stalled queue.
    static uint64 GetSkinCommitProgressGeneration();

    // True after this pawn has received its queue turn and begun bounded
    // candidate/native work. Used with the shared heartbeat so unrelated bot
    // progress cannot conceal one individually stalled transaction.
    static bool IsSkinCommitActivelyWorking(
        AFortPlayerPawnAthena* Pawn);

    // Drops any queued cosmetic work for one pawn during terminal bot cleanup
    // or a pawn/world lifecycle transition.
    static void CancelSkinCommit(
        AFortPlayerPawnAthena* Pawn);

    // ---- Death --------------------------------------------------------------------
    // Kills a pawn through the native death pipeline (ForceKill) so kill
    // credit / eliminations / alive counts / victory all flow through the
    // exact same path as a real player death. Killer may be nullptr
    // (storm / environmental deaths give no kill credit).
    static bool KillPawn(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* KillerPC, AActor* DamageCauser);

    // ---- Cosmetics -------------------------------------------------------------------
    // Applies default character parts + hero so the AI replicates as a valid
    // player-like entity on every version. Returns true only after a complete
    // default or resident-donor outfit was committed.
    static bool ApplyDefaultCosmetics(
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn,
        EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly);

    // Applies only the fixed HID_001/head/body/no-backpack defaults. Unlike
    // universal PlayerAI fallback, this never borrows a real player's outfit.
    // If those defaults are unavailable, the pawn keeps its engine appearance.
    static bool ApplyFixedDefaultCosmetics(
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* Pawn,
        EPlayerAICosmeticLoadPolicy LoadPolicy =
            EPlayerAICosmeticLoadPolicy::ResidentOnly);

    // Reset all cached feature lookups (new match / map).
    static void ResetCaches();
};
