#pragma once
#include "SupportTypes.h"

class AIDebugLogger
{
public:
    static inline bool bVerbose = false;

    static void Log(const char* Category, const char* Format, ...);

    static void Verbose(const char* Category, const char* Format, ...);

    static void MissingFeature(const char* FeatureName, const char* FallbackDescription);

    static void Error(const char* Category, const char* Format, ...);
};
