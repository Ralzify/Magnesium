#pragma once
#include "../../pch.h"
#include "FortWeapon.h"

class AFortCreativeMoveTool : public AFortWeapon
{
public:
    UCLASS_COMMON_MEMBERS(AFortCreativeMoveTool);

    DefUHookOg(ServerStartInteracting_);
    DefUHookOg(ServerDuplicateStartInteracting_);
    DefUHookOg(ServerSpawnActorWithTransform_);

    InitHooks;
};

class APhoneToolActorTargetMode
{
public:
    static void Hook();
};
