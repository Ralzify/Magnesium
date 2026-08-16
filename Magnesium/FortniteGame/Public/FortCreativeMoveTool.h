#pragma once
#include "../../pch.h"
#include "FortWeapon.h"

// Creative phone RPCs are present in client builds whose dedicated-server
// implementations are stripped. The implementation resolves every mutable
// field through reflection so the same wrapper can span the pre- and post-LWC
// layouts.
class AFortCreativeMoveTool : public AFortWeapon
{
public:
    UCLASS_COMMON_MEMBERS(AFortCreativeMoveTool);

    DefUHookOg(ServerStartInteracting_);
    DefUHookOg(ServerDuplicateStartInteracting_);
    DefUHookOg(ServerSpawnActorWithTransform_);

    InitHooks;
};

// Newer phone implementations move the interaction state off the weapon and
// onto this target-mode actor. It deliberately has no static SDK class macro:
// Hook() discovers the class/capabilities at runtime.
class APhoneToolActorTargetMode
{
public:
    static void Hook();
};
