#pragma once
#include <atomic>
// ============================================================================
// Magnesium PlayerAI - MagnesiumPlayerAISettings
//
// The user facing settings for the universal PlayerAI system. Enable AIs is
// forced on from the existing Player Bot Tab of the Magnesium configuration UI.
//
// NOTE: the tab keeps its existing "Player Bot" name because that is part
// of the existing UI, but internally this system is named PlayerAI and is
// completely separate from the existing bot command (which only spawns a
// player pawn and remains untouched).
// ============================================================================

struct MagnesiumPlayerAISettings
{
    static std::atomic_bool bEnableAIs;
};
