#include "pch.h"
#include "../Public/BuildingFoundation.h"

uint64_t SelectAndSetupMyBuildingLevel_ = 0;
uint64_t StreamInMyBuilding_ = 0;

void ABuildingFoundation::SetDynamicFoundationEnabled_(UObject* Context, FFrame& Stack)
{
	auto Foundation = (ABuildingFoundation*)Context;
	bool bEnabled;
	Stack.StepCompiledIn(&bEnabled);
	Stack.IncrementCode();

	if (Foundation->HasbFoundationEnabled())
	{
		auto OldEnabled = Foundation->bFoundationEnabled;
		Foundation->bFoundationEnabled = bEnabled;

		if (Foundation->GetFunction("OnRep_FoundationEnabled"))
			Foundation->OnRep_FoundationEnabled(OldEnabled);
	}

	if (Foundation->HasDynamicFoundationRepData())
	{
		Foundation->DynamicFoundationRepData.EnabledState = bEnabled ? 1 : 2;
		Foundation->OnRep_DynamicFoundationRepData();
	}
	if (Foundation->HasFoundationEnabledState())
	{
		Foundation->FoundationEnabledState = bEnabled ? 1 : 2;

		if (Foundation->GetFunction("OnRep_FoundationEnabledState"))
			Foundation->OnRep_FoundationEnabledState();
	}

	if (bEnabled && Foundation->HasLevelToStream() &&
		Foundation->LevelToStream.ComparisonIndex == 0 && SelectAndSetupMyBuildingLevel_)
	{
		auto SelectAndSetupMyBuildingLevel = (bool (*)(ABuildingFoundation*, void*))SelectAndSetupMyBuildingLevel_;

		if (SelectAndSetupMyBuildingLevel(Foundation, nullptr) && StreamInMyBuilding_)
		{
			auto StreamInMyBuilding = (void (*)(ABuildingFoundation*, bool))StreamInMyBuilding_;
			StreamInMyBuilding(Foundation, false);
		}
	}
}

void ABuildingFoundation::SetDynamicFoundationTransform_(UObject* Context, FFrame& Stack)
{
	auto Foundation = (ABuildingFoundation*)Context;
	auto& Transform = Stack.StepCompiledInRef<FTransform>();
	Stack.IncrementCode();

	Foundation->DynamicFoundationTransform = Transform;
	if (Foundation->HasDynamicFoundationRepData())
	{
		Foundation->DynamicFoundationRepData.Rotation = Transform.Rotation.Rotator();
		Foundation->DynamicFoundationRepData.Translation = Transform.Translation;
	}
	Foundation->StreamingData.FoundationLocation = Transform.Translation;
	//Foundation->StreamingData.BoundingBox = Foundation->StreamingBoundingBox;
	if (Foundation->HasDynamicFoundationRepData())
		Foundation->OnRep_DynamicFoundationRepData();
}

void ABuildingFoundation::Hook()
{
	if (!GetDefaultObj())
		return;

	SelectAndSetupMyBuildingLevel_ = FindSelectAndSetupMyBuildingLevel();
	StreamInMyBuilding_ = FindStreamInMyBuilding();

	Utils::ExecHook(GetDefaultObj()->GetFunction("SetDynamicFoundationEnabled"), SetDynamicFoundationEnabled_);
	Utils::ExecHook(GetDefaultObj()->GetFunction("SetDynamicFoundationTransform"), SetDynamicFoundationTransform_);
}
