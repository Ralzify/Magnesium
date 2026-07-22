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

    // Rotation is NOT the same type on every build:
    //
    //   Season X (10.40): Translation FVector @ 0x00, Rotation FQuat    @ 0x10
    //   13.40 onwards:    Rotation    FRotator @ 0x00, Translation FVector @ 0x0C
    //
    // Reflection resolves the offset either way, but the TYPE is baked into
    // the DEFINE_STRUCT_PROP above. Writing a 12-byte FRotator into the
    // 16-byte FQuat leaves W holding whatever was there before, which
    // denormalises the quaternion into an arbitrary rotation - that is the
    // upside-down moveable island on 10.40.
    //
    // Detected structurally instead of by version number: FQuat needs
    // 16-byte alignment, so that layout has to put the 12-byte Translation
    // first to fill the gap. Rotation sitting AFTER Translation therefore
    // means it is the quaternion form.
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

    // Writes the rotation in whichever form this build expects. Takes the
    // quaternion because that is what FTransform carries - converting to
    // Euler first would throw away the precision we need on the quat builds.
    void SetRotationFromQuat(FQuat& Quat) const
    {
        if (!HasRotation())
            return;

        if (IsRotationQuat())
        {
            // FQuat::operator= memcpy's exactly Size() bytes, so this stays
            // correct across the 0x10 / 0x20 (LWC) layouts too.
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
    //DEFINE_FUNC(SetDynamicFoundationTransform, void);
    DEFINE_FUNC(SetDynamicFoundationEnabled, void);
    DEFINE_FUNC(OnRep_ServerStreamedInLevel, void);
    DEFINE_FUNC(OnRep_LevelToStream, void);
    DEFINE_FUNC(OnRep_FoundationEnabledState, void);
    DEFINE_FUNC(OnRep_FoundationEnabled, void);

    static void SetDynamicFoundationEnabled_(UObject*, FFrame&);
    static void SetDynamicFoundationTransform_(UObject*, FFrame&);

    InitHooks;
};
