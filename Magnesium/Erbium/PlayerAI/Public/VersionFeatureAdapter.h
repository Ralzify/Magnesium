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
    static void ApplyRandomSkin(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn);

    // Amortizes current-build skin discovery across server ticks. This never
    // loads packages and never walks the complete UObject array in one frame.
    static void TickCosmeticCache();

    // ---- Death --------------------------------------------------------------------
    // Kills a pawn through the native death pipeline (ForceKill) so kill
    // credit / eliminations / alive counts / victory all flow through the
    // exact same path as a real player death. Killer may be nullptr
    // (storm / environmental deaths give no kill credit).
    static bool KillPawn(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* KillerPC, AActor* DamageCauser);

    // ---- Cosmetics -------------------------------------------------------------------
    // Applies default character parts + hero so the AI replicates as a valid
    // player-like entity on every version. Custom skins only when supported.
    static void ApplyDefaultCosmetics(AFortPlayerStateAthena* PlayerState, AFortPlayerPawnAthena* Pawn);

    // Reset all cached feature lookups (new match / map).
    static void ResetCaches();
};
