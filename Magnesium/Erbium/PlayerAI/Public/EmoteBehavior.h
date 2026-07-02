#pragma once
// ============================================================================
// Magnesium PlayerAI - EmoteBehavior
//
// Emote handling. Emotes are only played when the current version supports
// them (checked through the VersionFeatureAdapter). When emotes are not
// supported the AI simply keeps walking / idling / turning / jumping - no
// fallback animations, no crash, no freeze.
// ============================================================================
#include "PlayerAIController.h"

class EmoteBehavior
{
public:
    // Tries to start an emote. Returns false when emotes are unsupported
    // (the caller continues with normal movement behavior).
    static bool TryStartEmote(PlayerAIController& AI, float Now);

    // Stops an emote naturally after its duration.
    static void StopEmote(PlayerAIController& AI);
};
