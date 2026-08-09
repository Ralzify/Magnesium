#pragma once
// ============================================================================
// Magnesium PlayerAI - AINameGenerator
//
// Random, human looking display names for PlayerAI players. Completely
// separate from the existing bot command naming (FConfiguration::BotName /
// "Anonymous [xxx]") - this module never reads or writes those settings.
// ============================================================================
#include "SupportTypes.h"
#include <string>

class AINameGenerator
{
public:
    // A fresh random display name, unique for this match.
    static std::string NextName();

    // Reset per-match uniqueness tracking (called on match end / new match).
    static void Reset();
};

