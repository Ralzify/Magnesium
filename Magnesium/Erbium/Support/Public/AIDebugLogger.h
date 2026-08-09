#pragma once
// ============================================================================
// Magnesium PlayerAI - AIDebugLogger
//
// Clean logging for the PlayerAI system. All log lines use "PlayerAI"
// terminology (never "bot") so they cannot be confused with the existing
// bot command output.
// ============================================================================
#include "SupportTypes.h"

class AIDebugLogger
{
public:
    // Master switch for verbose per-AI logging. Errors/fallbacks always log.
    static inline bool bVerbose = false;

    // General log: [PlayerAI][Category] message
    static void Log(const char* Category, const char* Format, ...);

    // Verbose log, only printed when bVerbose is enabled (state changes,
    // target changes, loot decisions, ... - very chatty with ~99 AI).
    static void Verbose(const char* Category, const char* Format, ...);

    // A required feature is missing on this version - logged once per
    // feature name, then the caller is expected to use a safe fallback.
    static void MissingFeature(const char* FeatureName, const char* FallbackDescription);

    // An error that was handled with a safe fallback (never crashes the match).
    static void Error(const char* Category, const char* Format, ...);
};

