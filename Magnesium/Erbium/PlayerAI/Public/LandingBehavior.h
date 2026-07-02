#pragma once
// ============================================================================
// Magnesium PlayerAI - LandingBehavior
//
// Landing spot selection + glide/landing handling. Landing spots are derived
// from generic map data: loot container locations are clustered into "loot
// areas" which works on every map and version (POI data is optional flavor).
// AI spread across clusters; hot-droppers pick dense/popular clusters, safer
// profiles pick remote ones.
// ============================================================================
#include "PlayerAIController.h"
#include <vector>

struct FPlayerAIWorldSnapshot;

struct FPlayerAILandingCluster
{
    FVector Center{};
    int LootCount = 0;
};

class LandingBehavior
{
public:
    // Builds (or rebuilds) the landing cluster table from world loot data.
    static void BuildClusters(FPlayerAIWorldSnapshot& World);

    // Picks a landing target for an AI based on its skill profile.
    static FVector PickLandingTarget(PlayerAIController& AI, FPlayerAIWorldSnapshot& World);

    // Think step while Gliding/Landing. Returns true once landed.
    static bool Think(PlayerAIController& AI, float Now, FPlayerAIWorldSnapshot& World);

    static void Reset();

    static std::vector<FPlayerAILandingCluster>& GetClusters();
};
