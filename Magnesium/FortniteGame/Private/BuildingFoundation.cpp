#include "pch.h"
#include "../Public/BuildingFoundation.h"

uint64_t SelectAndSetupMyBuildingLevel_ = 0;
uint64_t StreamInMyBuilding_ = 0;

namespace
{
    void WakeFoundationReplication(ABuildingFoundation* Foundation)
    {
        if (!Foundation)
            return;
        if (Foundation->GetFunction("FlushNetDormancy"))
            Foundation->FlushNetDormancy();
        if (Foundation->GetFunction("ForceNetUpdate"))
            Foundation->ForceNetUpdate();
    }

    void ApplyDynamicFoundationTransform(ABuildingFoundation* Foundation, FTransform& Transform)
    {
        if (!Foundation)
            return;

        if (Foundation->HasDynamicFoundationTransform())
        {
            const int32 TransformSize = FTransform::Size();
            if (TransformSize > 0 && TransformSize <= static_cast<int32>(sizeof(FTransform)) &&
                SDK::MemReadable(&Foundation->DynamicFoundationTransform, TransformSize))
            {
                memcpy(&Foundation->DynamicFoundationTransform, &Transform, TransformSize);
            }
        }

        bool bUpdatedRepData = false;
        if (Foundation->HasDynamicFoundationRepData() &&
            FDynamicBuildingFoundationRepData::StaticStruct())
        {
            auto& RepData = Foundation->DynamicFoundationRepData;
            if (FDynamicBuildingFoundationRepData::HasRotation())
            {
                RepData.SetRotationFromQuat(Transform.Rotation);
                bUpdatedRepData = true;
            }
            if (FDynamicBuildingFoundationRepData::HasTranslation())
            {
                RepData.Translation = Transform.Translation;
                bUpdatedRepData = true;
            }
        }

        if (Foundation->HasStreamingData() && FBuildingFoundationStreamingData::StaticStruct())
        {
            auto& StreamingData = Foundation->StreamingData;
            if (FBuildingFoundationStreamingData::HasFoundationLocation())
            {
                StreamingData.FoundationLocation = Transform.Translation;
            }

            // FBox is opaque in the SDK, so copy its reflected size to cover both the float and LWC layouts.
            if (FBuildingFoundationStreamingData::HasBoundingBox() &&
                Foundation->HasStreamingBoundingBox())
            {
                auto BoxStruct = FBox::StaticStruct();
                const int32 BoxSize = BoxStruct ? BoxStruct->GetPropertiesSize() : 0;
                if (BoxSize > 0 && BoxSize <= 0x100)
                {
                    memcpy(&StreamingData.BoundingBox, &Foundation->StreamingBoundingBox, BoxSize);
                }
            }
        }

        if (bUpdatedRepData && Foundation->GetFunction("OnRep_DynamicFoundationRepData"))
        {
            Foundation->OnRep_DynamicFoundationRepData();
        }
    }
}

void ABuildingFoundation::SetDynamicFoundationEnabled_(UObject* Context, FFrame& Stack)
{
    auto Foundation = (ABuildingFoundation*)Context;
    bool bEnabled = false;
    Stack.StepCompiledIn(&bEnabled);
    Stack.IncrementCode();
    if (!Foundation || !SDK::MemReadable(Foundation, sizeof(UObject)))
    {
        return;
    }

    if (Foundation->HasbFoundationEnabled())
    {
        auto OldEnabled = Foundation->bFoundationEnabled;
        Foundation->bFoundationEnabled = bEnabled;

        if (Foundation->GetFunction("OnRep_FoundationEnabled"))
            Foundation->OnRep_FoundationEnabled(OldEnabled);
    }

    if (Foundation->HasDynamicFoundationRepData())
    {
        if (FDynamicBuildingFoundationRepData::StaticStruct() &&
            FDynamicBuildingFoundationRepData::HasEnabledState())
        {
            Foundation->DynamicFoundationRepData.EnabledState = bEnabled ? 1 : 2;
            if (Foundation->GetFunction("OnRep_DynamicFoundationRepData"))
            {
                Foundation->OnRep_DynamicFoundationRepData();
            }
        }
    }
    if (Foundation->HasFoundationEnabledState())
    {
        Foundation->FoundationEnabledState = bEnabled ? 1 : 2;

        if (Foundation->GetFunction("OnRep_FoundationEnabledState"))
            Foundation->OnRep_FoundationEnabledState();
    }

    if (!bEnabled)
    {
        WakeFoundationReplication(Foundation);
        return;
    }

    bool bHasTransform = false;
    FTransform Transform{};
    const int32 AuthoredTransformSize = FTransform::Size();
    if (Foundation->HasDynamicFoundationTransform() && AuthoredTransformSize > 0 &&
        AuthoredTransformSize <= static_cast<int32>(sizeof(FTransform)) && SDK::MemReadable(
            &Foundation->DynamicFoundationTransform, AuthoredTransformSize))
    {
        memcpy(&Transform, &Foundation->DynamicFoundationTransform, AuthoredTransformSize);
        bHasTransform = true;
    }
    else if (Foundation->GetFunction("GetTransform"))
    {
        Transform = Foundation->GetTransform();
        bHasTransform = true;
    }
    if (bHasTransform)
        ApplyDynamicFoundationTransform(Foundation, Transform);

    if (!Foundation->HasLevelToStream())
    {
        WakeFoundationReplication(Foundation);
        return;
    }

    bool bSetupSucceeded = true;
    bool bHasLevelToStream = Foundation->LevelToStream.IsValid();
    if (!bHasLevelToStream && SelectAndSetupMyBuildingLevel_)
    {
        auto SelectAndSetupMyBuildingLevel = (bool (*)(ABuildingFoundation*, void*))
                SelectAndSetupMyBuildingLevel_;
        bSetupSucceeded = SelectAndSetupMyBuildingLevel(Foundation, nullptr);
        bHasLevelToStream = Foundation->LevelToStream.IsValid();
    }

    const bool bStreamedIn = bSetupSucceeded && bHasLevelToStream && StreamInMyBuilding_;
    if (bStreamedIn)
    {
        auto StreamInMyBuilding = (void (*)(ABuildingFoundation*, bool))
                StreamInMyBuilding_;
        StreamInMyBuilding(Foundation, false);
    }

    SDK::DbgLog("  [Foundation] %s enabled level=%s setup=%d streamed-in=%d flag=%d\n",
        Foundation->Name.ToString().c_str(),
        bHasLevelToStream ? Foundation->LevelToStream.ToString().c_str() : "none", bSetupSucceeded,
        bStreamedIn, Foundation->HasbServerStreamedInLevel() && Foundation->bServerStreamedInLevel);

    WakeFoundationReplication(Foundation);
}

void ABuildingFoundation::SetDynamicFoundationTransform_(UObject* Context, FFrame& Stack)
{
    auto Foundation = (ABuildingFoundation*)Context;
    auto& Transform = Stack.StepCompiledInRef<FTransform>();
    Stack.IncrementCode();
    if (!Foundation || !SDK::MemReadable(Foundation, sizeof(UObject)))
    {
        return;
    }

    SDK::DbgLog("  [Foundation] %s transform\n", Foundation->Name.ToString().c_str());
    ApplyDynamicFoundationTransform(Foundation, Transform);
    WakeFoundationReplication(Foundation);
}

void ABuildingFoundation::Hook()
{
    if (!GetDefaultObj())
        return;

    SelectAndSetupMyBuildingLevel_ = FindSelectAndSetupMyBuildingLevel();
    StreamInMyBuilding_ = FindStreamInMyBuilding();

    auto SetEnabled = GetDefaultObj()->GetFunction("SetDynamicFoundationEnabled");
    if (SetEnabled)
        Utils::ExecHook(SetEnabled, SetDynamicFoundationEnabled_);
    auto SetTransform = GetDefaultObj()->GetFunction("SetDynamicFoundationTransform");
    if (SetTransform)
        Utils::ExecHook(SetTransform, SetDynamicFoundationTransform_);
}
