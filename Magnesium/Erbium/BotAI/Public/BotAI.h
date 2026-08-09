#pragma once
// ============================================================================
// Magnesium - BotAI
//
// Drives the player bots created by the `spawnbot` / `bot` cheat command so
// they behave like live participants instead of standing dormant where they
// spawned.
//
// Scope and ownership
//   - BotAI never spawns or destroys anything. It discovers the controllers
//     the spawnbot command already tracks
//     (AFortPlayerControllerAthena::IsCheatSpawnedBotController) and steers
//     the pawn those controllers already possess. Removing a bot with
//     `delbot`, killing it, or ending the match drops it from BotAI by
//     itself.
//   - Native AI (henchmen, guards, bosses, wildlife, playlist bots) is never
//     touched: those controllers are not spawnbot controllers.
//   - Everything version specific goes through VersionFeatureAdapter, so a
//     build that lacks a feature falls back instead of failing.
//
// Current behavior (see BotAI.cpp for the state machine)
//   Bots wander the map at random and prioritize being inside the safe zone:
//   a bot caught outside - or close to the edge of a shrinking circle -
//   drops what it is doing and rotates in. Walking, running, swimming and
//   the matching animations are all the pawn's own native movement; BotAI
//   only supplies movement input and the native sprint ability.
//
// The bus/skydive path is implemented but only engages for bots that
// actually end up aboard an aircraft, which the spawnbot command does not do
// today. It is the groundwork for full 100-player AI lobbies.
// ============================================================================
#include "../../Support/Public/SupportTypes.h"
#include <atomic>

class UNetDriver;

struct BotAISettings
{
    // Master toggle, owned by the Player Bot tab of the configuration UI.
    // Off means every spawnbot bot stays exactly as dormant as before.
    static std::atomic_bool bEnabled;

    // Rotate into the safe zone instead of wandering freely. Off makes bots
    // wander the whole map and ignore the storm.
    static std::atomic_bool bSeekSafeZone;

    // Occasional jumps / short pauses so bots do not read as pure pathing.
    static std::atomic_bool bIdleFlourishes;

    // Let bots take the native movement path by giving their controller a
    // ULocalPlayer, which is what makes the pawn's character movement run.
    // Off falls every bot back to BotAI's own swept walking, which works
    // everywhere but cannot swim. Turn this off if a build reacts badly to
    // a server-side controller claiming to be local.
    static std::atomic_bool bNativeMovement;

    // Investigation aid, not a feature. Switches the swept driver off and
    // steers bots with AddMovementInput alone, logging once a second per
    // bot whether native character movement is running for them at all.
    // Bots are expected to stand still while this is on.
    static std::atomic_bool bMovementDiagnostics;
};

class BotAI
{
public:
    // Sole entry point, called once per frame from the Magnesium server tick
    // (UNetDriver::TickFlush). Safe to call every tick in every phase: it
    // returns early when the toggle is off and nothing is being driven.
    static void OnServerTick(UNetDriver* Driver, float DeltaSeconds);

    // Drops every tracked bot. Safe to call repeatedly.
    static void Shutdown(const char* Reason);

    // True when BotAI is currently driving this controller. Registered with
    // VersionFeatureAdapter so shared support code can ask without depending
    // on this module.
    static bool IsManaged(const AFortPlayerControllerAthena* PC);
    static bool HasManaged();

    static int GetActiveCount();

    // Status line for the configuration UI (Player Bot tab).
    static const char* GetStatusLine();
};

