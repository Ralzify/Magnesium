#pragma once
// ============================================================================
// Magnesium PlayerAI - MagnesiumPlayerAISettings
//
// The user facing settings for the universal PlayerAI system. Only ONE
// setting is required: "Enable AIs", surfaced as a toggle inside the
// existing Player Bot Tab of the Magnesium configuration UI.
//
// NOTE: the tab keeps its existing "Player Bot" name because that is part
// of the existing UI, but internally this system is named PlayerAI and is
// completely separate from the existing bot command (which only spawns a
// player pawn and remains untouched).
// ============================================================================

struct MagnesiumPlayerAISettings
{
    // The one and only required setting. Off by default: Magnesium behaves
    // exactly like before and no PlayerAI players ever spawn.
    static inline bool bEnableAIs = false;

    // Optional: verbose PlayerAI debug logging (not required for use).
    static inline bool bVerboseLogging = false;
};
