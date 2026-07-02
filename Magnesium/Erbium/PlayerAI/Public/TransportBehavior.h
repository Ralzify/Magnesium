#pragma once
// ============================================================================
// Magnesium PlayerAI - TransportBehavior
//
// Bus/transport phase: enter the transport through the same native path
// real players use, optionally thank the driver (only when the version
// supports it), pick a landing spot and jump at a natural, staggered time.
// Every step has a safe fallback when the native transport feature is
// unavailable on a version.
// ============================================================================
#include "PlayerAIController.h"

struct FPlayerAIWorldSnapshot;

class TransportBehavior
{
public:
    // Called once when the transport phase starts for this AI.
    static void OnTransportPhaseStarted(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);

    // Think step during WaitingForTransport / InTransport / Jumping states.
    static void Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);
};
