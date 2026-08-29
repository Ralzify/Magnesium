#pragma once
#include "SupportTypes.h"
#include <string>

class AINameGenerator
{
public:
    static std::string NextName();

    static void Reset();
};
