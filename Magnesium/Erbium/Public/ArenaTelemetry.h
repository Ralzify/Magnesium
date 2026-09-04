#pragma once

#include <cstdint>

class AFortGameMode;
class AFortGameStateAthena;
class AFortPlayerControllerAthena;
class AFortPlayerStateAthena;

// Server-authoritative Arena match reporting for ATLAS Backend.
//
// Every public entry point is called from Magnesium's game-thread hooks. The
// implementation only snapshots small values there; JSON construction, HTTP,
// acknowledgements, and retries are owned by a dedicated worker thread.
namespace ArenaTelemetry
{
    // Pumps worker startup and bounded queue retries. Called once per server
    // frame by the authoritative match-lifecycle tick.
    void Tick() noexcept;

    // These lifecycle edges are idempotent for a match generation. Unsupported
    // playlists do not create a reporting session.
    void OnMatchStarted(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState) noexcept;
    void OnMatchEnded(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState) noexcept;

    // Registers a real connected player after native startup/team assignment.
    // Players already present are also enumerated when a match starts.
    void RegisterPlayer(
        AFortPlayerControllerAthena* PlayerController) noexcept;

    // Called only from the finalized, credited permanent-elimination path.
    bool RecordCreditedElimination(
        AFortGameMode* GameMode,
        AFortPlayerStateAthena* KillerPlayerState,
        AFortPlayerStateAthena* VictimPlayerState,
        std::uint64_t VictimLifeId,
        bool MatchWasLive,
        bool VictimWasAliveParticipant) noexcept;

    // Sends the client-only tournament elimination presentation. This never
    // mutates, queues, or persists Arena points. Presentation is session-local
    // and remains available while saved progression is paused. The
    // controller/placement are used only for a guarded +20 placement-RPC
    // fallback when schema validation rejects before ProcessEvent begins.
    bool NotifyTournamentEliminationVisual(
        AFortPlayerControllerAthena* KillerPlayerController,
        AFortPlayerStateAthena* KillerPlayerState,
        int CurrentPlacement) noexcept;

    // Sends the placement/points toast through the reflected client RPC.
    // This is presentation only; callers remain responsible for scoring and
    // persistence, and can safely fall back on older generated wrappers.
    bool NotifyTournamentPlacementVisual(
        AFortPlayerControllerAthena* PlayerController,
        int Placement,
        int PointsEarned,
        bool* DedicatedRpcExpected = nullptr,
        bool AllowTypedStatFallback = true) noexcept;

    // Queues the one-shot private bus-fare event after an Arena player leaves
    // the bus. Saved sessions use the backend-authored fee; paused sessions
    // use a zero-Hype, zero-fare match-local tournament state. The client
    // observes native MatchEntered presentation before applying its repair.
    void NotifyArenaBusFareVisual(
        AFortGameMode* GameMode,
        AFortPlayerControllerAthena* PlayerController) noexcept;

    // Mirrors a signed additive Arena delta shown by Magnesium (for example,
    // setpoints or randomized late-game Hype). ATLAS still stages it with the
    // rest of the match and applies it only when capture remains enabled
    // through session end.
    void RecordPointsAdjustment(
        AFortGameMode* GameMode,
        AFortPlayerControllerAthena* PlayerController,
        int PointsDelta) noexcept;

    // Mirrors Magnesium's authoritative Arena placement milestone awards.
    void RecordPlacementMilestone(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        int PlayersBeforeDeath,
        bool IsFinalizedLiveDeath,
        bool UseFallbackPresentation) noexcept;

    // Kept public and side-effect free so the playlist/scoring policy can be
    // regression-tested without creating a live Unreal world.
    int GetPlacementPointsForPlayersRemaining(
        int PlayersRemaining) noexcept;
    bool ResolveCanonicalArenaPlaylist(
        const char* ActivePlaylistObjectName,
        double FortniteVersion,
        const char** CanonicalPath = nullptr) noexcept;
    bool IsCanonicalArenaMatch(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState) noexcept;
}
