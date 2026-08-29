#pragma once
#include "../../pch.h"

struct FBox
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FBox);
};

struct FDynamicBuildingFoundationRepData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FDynamicBuildingFoundationRepData);

    DEFINE_STRUCT_PROP(Rotation, FRotator);
    DEFINE_STRUCT_PROP(Translation, FVector);
    DEFINE_STRUCT_PROP(EnabledState, uint8);

    // Season X (10.40) is Translation FVector @0x00 + Rotation FQuat @0x10; 13.40+ is Rotation FRotator @0x00 + Translation FVector @0x0C.
    static bool IsRotationQuat()
    {
        static int Cached = -1;

        if (Cached == -1)
        {
            Cached = (HasRotation() && HasTranslation() && Rotation__Offset > Translation__Offset) ? 1 : 0;

            SDK::DbgLog("  [Foundation] DynamicFoundationRepData.Rotation is %s (Rotation=0x%X Translation=0x%X)\n",
                Cached ? "FQuat" : "FRotator", Rotation__Offset, Translation__Offset);
        }

        return Cached == 1;
    }

    void SetRotationFromQuat(FQuat& Quat) const
    {
        if (!HasRotation())
            return;

        if (IsRotationQuat())
        {
            // FQuat::operator= memcpies exactly Size() bytes, so this holds across the 0x10 and 0x20 LWC layouts.
            GetFromOffset<FQuat>(this, Rotation__Offset) = Quat;
            return;
        }

        Rotation = Quat.Rotator();
    }
};

struct FBuildingFoundationStreamingData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FBuildingFoundationStreamingData);

    DEFINE_STRUCT_PROP(FoundationLocation, FVector);
    DEFINE_STRUCT_PROP(BoundingBox, FBox);
};

class ABuildingFoundation : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(ABuildingFoundation);

    DEFINE_PROP(FoundationEnabledState, uint8);
    DEFINE_PROP(DynamicFoundationRepData, FDynamicBuildingFoundationRepData);
    DEFINE_PROP(DynamicFoundationTransform, FTransform);
    DEFINE_PROP(StreamingData, FBuildingFoundationStreamingData);
    DEFINE_PROP(StreamingBoundingBox, FBox);
    DEFINE_PROP(DynamicFoundationType, uint8);
    DEFINE_PROP(LevelToStream, FName);
    DEFINE_BITFIELD_PROP(bServerStreamedInLevel);
    DEFINE_BITFIELD_PROP(bFoundationEnabled);

    DEFINE_FUNC(OnRep_DynamicFoundationRepData, void);
    DEFINE_FUNC(SetDynamicFoundationEnabled, void);
    DEFINE_FUNC(OnRep_ServerStreamedInLevel, void);
    DEFINE_FUNC(OnRep_LevelToStream, void);
    DEFINE_FUNC(OnRep_FoundationEnabledState, void);
    DEFINE_FUNC(OnRep_FoundationEnabled, void);

    static void SetDynamicFoundationEnabled_(UObject*, FFrame&);
    static void SetDynamicFoundationTransform_(UObject*, FFrame&);

    InitHooks;
};
