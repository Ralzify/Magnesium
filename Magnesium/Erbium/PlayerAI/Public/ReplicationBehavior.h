#pragma once
// ============================================================================
// Magnesium PlayerAI - ReplicationBehavior
//
// Replication glue for PlayerAI entities. PlayerAI players are real player
// pawns / player states / controllers spawned server side, so position,
// rotation, movement, health, shield, inventory, equipped weapon, damage,
// death and eliminations replicate through the existing Magnesium
// networking (ServerReplicateActors / replication graph / Iris) exactly
// like any other player-like entity - the server stays authoritative.
// This module only applies per-actor replication settings and pushes
// relevant OnRep notifications; it is completely separate from native NPC /
// henchman / guard / boss / wildlife replication.
//
// TODO: connect this to the Magnesium movement replication system if
//       smoother client interpolation is desired for a specific version.
// ============================================================================
#include "PlayerAITypes.h"
#include "../../../FortniteGame/Public/FortPlayerControllerAthena.h"

class ReplicationBehavior
{
public:
    // Makes a newly spawned PlayerAI pawn replicate reliably to all clients.
    static void SetupPawnReplication(AFortPlayerPawnAthena* Pawn);

    // Pushes replication updates after direct server side state changes
    // (health/shield writes from simulated combat, storm damage, healing).
    static void PushHealthShieldUpdate(AFortPlayerPawnAthena* Pawn);

    // Forces a movement replication update when the AI teleports (stuck
    // recovery) so clients do not badly desync.
    static void PushTeleportUpdate(AFortPlayerPawnAthena* Pawn);
};
