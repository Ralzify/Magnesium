#include "pch.h"
#include "../Public/NetDriver.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Public/Finders.h"
#include "../../Erbium/Public/GUI.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../Public/AbilitySystemComponent.h"
#include "../../Erbium/PlayerAI/Public/MagnesiumPlayerAIIntegration.h"

#include <cmath>

uint32_t NetworkObjectListOffset = 0;
uint32_t ReplicationFrameOffset = 0;
uint32_t ClientWorldPackageNameOffset = 0;
uint32_t DestroyedStartupOrDormantActorsOffset = 0;
uint32_t DestroyedStartupOrDormantActorGUIDsOffset = 0;
uint32_t ClientVisibleLevelNamesOffset = 0;

std::unordered_map<UNetConnection*, TArray<FNetViewer*>> ViewerMap;


const ULevel* GetLevel(const AActor* Actor)
{
	auto Outer = Actor->Outer;

	while (Outer)
	{
		if (Outer->Class == ULevel::StaticClass())
			return (ULevel*)Outer;
		else
			Outer = Outer->Outer;
	}

	return nullptr;
}


void BuildViewerMap(UNetDriver* Driver)
{
	//Log(L"PC");
	for (auto& Conn : Driver->ClientConnections)
	{
		auto Owner = Conn->OwningActor;
		if (Owner)
		{
			auto OutViewTarget = Owner;
			if (auto Controller = Conn->PlayerController)
				if (auto ViewTarget = Controller->GetViewTarget())
						OutViewTarget = ViewTarget;

			Conn->ViewTarget = OutViewTarget;

			TArray<FNetViewer*> Viewers;
			Viewers.Reserve(1 + Conn->Children.Num());
			Viewers.Add(FNetViewer::Create(Conn));

			for (auto& Child : Conn->Children)
			{
				if (auto Controller = Child->PlayerController)
				{
					Child->ViewTarget = Controller->GetViewTarget();
					Viewers.Add(FNetViewer::Create(Child));
				}
				else
					Child->ViewTarget = nullptr;
			}
			ViewerMap[Conn] = Viewers;
		}
		else
		{
			Conn->ViewTarget = nullptr;
			for (auto& Child : Conn->Children)
				Child->ViewTarget = nullptr;
		}
	}
}

static FNetworkObjectList& GetNetworkObjectList(UNetDriver* Driver)
{
	return *(*(class TSharedPtr<FNetworkObjectList>*)(__int64(Driver) + NetworkObjectListOffset));
}

UNetConnection* IsActorOwnedByAndRelevantToConnection(const AActor* Actor, TArray<FNetViewer*>& ConnViewers, bool& bOutHasNullViewTarget)
{
	auto IsNetRelevantForIdx = FindIsNetRelevantForVft();
	if (IsNetRelevantForIdx == 0)
		return ConnViewers[0]->Connection;

	bool (*&IsRelevancyOwnerFor)(const AActor*, const AActor*, const AActor*, const AActor*) = decltype(IsRelevancyOwnerFor)(Actor->Vft[IsNetRelevantForIdx + 2]);
	AActor* (*&GetNetOwner)(const AActor*) = decltype(GetNetOwner)(Actor->Vft[IsNetRelevantForIdx + (VersionInfo.EngineVersion >= 4.19 ? 6 : 5)]);

	const AActor* ActorOwner = GetNetOwner(Actor);

	bOutHasNullViewTarget = false;

	for (auto& Viewer : ConnViewers)
	{
		auto Conn = Viewer->Connection;

		if (Conn->ViewTarget == nullptr)
		{
			bOutHasNullViewTarget = true;
		}

		if (ActorOwner == Conn->PlayerController ||
			(Conn->PlayerController && ActorOwner == Conn->PlayerController->Pawn) ||
			(Conn->ViewTarget && IsRelevancyOwnerFor(Conn->ViewTarget, Actor, ActorOwner, Conn->OwningActor)))
		{
			return Conn;
		}
	}

	return nullptr;
}

bool IsActorRelevantToConnection(const AActor* Actor, const TArray<FNetViewer*>& ConnectionViewers)
{
	auto IsNetRelevantForIdx = FindIsNetRelevantForVft();
	if (IsNetRelevantForIdx == 0)
		return true;

	bool (*&IsNetRelevantFor)(const AActor*, const AActor*, const AActor*, const FVector&) = decltype(IsNetRelevantFor)(Actor->Vft[IsNetRelevantForIdx]);

	for (auto& Viewer : ConnectionViewers)
	{
		if (!Viewer)
			continue;

		if (IsNetRelevantFor(Actor, Viewer->InViewer, Viewer->ViewTarget, Viewer->ViewLocation))
		{
			return true;
		}
	}

	return false;
}

bool IsLevelInitializedForActor(const UNetDriver* NetDriver, const AActor* InActor, UNetConnection* InConnection)
{
	static bool (*ClientHasInitializedLevelFor)(const UNetConnection*, const UObject*) = decltype(ClientHasInitializedLevelFor)(FindClientHasInitializedLevelFor());

	const bool bCorrectWorld = NetDriver->WorldPackage != nullptr && 
		(!ClientWorldPackageNameOffset || *(FName*)(__int64(InConnection) + ClientWorldPackageNameOffset) == NetDriver->WorldPackage->Name) && 
		(!ClientHasInitializedLevelFor || ClientHasInitializedLevelFor(InConnection, InActor));
	const bool bIsConnectionPC = (InActor == InConnection->PlayerController);
	return bCorrectWorld || bIsConnectionPC;
}

struct FPrioActor
{
	FNetworkObjectInfo* ActorInfo;
	UActorChannel* Channel;
	float Priority;
	bool bIsRelevant;
	bool bLevelInitializedForActor;

	bool operator<(FPrioActor& _Rhs)
	{
		return Priority < _Rhs.Priority;
	}
};

std::unordered_map<UNetConnection*, UEAllocatedVector<FPrioActor>> PriorityLists;

void (*GetActorLocation)(AActor*, FFrame&, FVector*);
void ServerReplicateActors(UNetDriver* Driver, float DeltaSeconds)
{
	if (!ReplicationFrameOffset)
		return;

	(*(int*)(__int64(Driver) + ReplicationFrameOffset))++;

	BuildViewerMap(Driver);
	if (ViewerMap.size() == 0)
		return;

	static FName ActorName = FName(L"Actor");

	auto& NetworkObjectList = GetNetworkObjectList(Driver);
	auto& ActiveNetworkObjects = NetworkObjectList.ActiveNetworkObjects;
	auto IsNetReady = (int32(*)(UNetConnection*, bool))FindIsNetReady();
	static auto CloseActorChannel = (void (*)(UActorChannel*, uint8_t))FindCloseActorChannel();

	for (auto& ViewerPair : ViewerMap)
	{
		auto& Conn = ViewerPair.first;
		auto& Viewers = ViewerPair.second;

		UEAllocatedVector<FPrioActor> List;
		List.reserve(ActiveNetworkObjects.Num());

		PriorityLists[Conn] = List;
	}

	auto TimeSeconds = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

	auto Scale = Driver->NetServerMaxTickRate / FConfiguration::MaxTickRate;
	FFrame FakeStack;
	for (auto& ActorInfo : ActiveNetworkObjects)
	{
		// if (/*!ActorInfo->bPendingNetUpdate && */TimeSeconds <= ActorInfo->NextUpdateTime)
		//	continue;

		auto Actor = ActorInfo->Actor;

		if (!Actor || /*!Actor->bActorInitialized || */ Actor->NetDriverName != Driver->NetDriverName)
		{
			continue;
		}

		auto Outer = Actor->Outer;
		if (Actor->bActorIsBeingDestroyed || (TUObjectArray::GetItemByIndex(Actor->Index)->Flags & ((1 << 29) | (1 << 21))) || Actor->RemoteRole == 0
			|| ((Actor->HasbNetStartup() ? Actor->bNetStartup : false) && Actor->NetDormancy == 4))
		{
			ActorInfo->NextUpdateTime = 43857458734643857485478534.f; // never gonna update lol
			// RemoveNetworkActor(&NetworkObjectList, Actor);
			continue;
		}

		if (!Actor->bReplicates)
			continue;

		bool bAnyRelevant = false;
		for (auto& ViewerPair : ViewerMap)
		{
			auto Conn = ViewerPair.first;
			auto& Viewers = ViewerPair.second;
			UActorChannel* Channel = nullptr;

			for (auto& Chan : Conn->OpenChannels)
			{
				if (Chan->Class != UActorChannel::StaticClass() || Chan->Actor != Actor)
					continue;

				Channel = Chan;
				break;
			}

			bool bLevelInitializedForActor = IsLevelInitializedForActor(Driver, Actor, Conn);
			if (!Channel && (!bLevelInitializedForActor || !IsActorRelevantToConnection(Actor, Viewers)))
			{
				continue;
			}

			auto PriorityConn = Conn;
			bool bDoCullCheck = true;

			if (Actor->bAlwaysRelevant || Actor->bTearOff)
				bDoCullCheck = false;

			if (Actor->bOnlyRelevantToOwner)
			{
				bDoCullCheck = false;
				bool bHasNullViewTarget = false;

				if (!(PriorityConn = IsActorOwnedByAndRelevantToConnection(Actor, Viewers, bHasNullViewTarget)))
				{
					if (!bHasNullViewTarget && Channel != NULL && Driver->GetTime() - Channel->GetRelevantTime() >= Driver->RelevantTimeout)
						CloseActorChannel(Channel, 3);

					continue;
				}
			}
			else if (VersionInfo.FortniteVersion >= 20) // its broken on legacy builds. idk why
			{
				if (VersionInfo.FortniteVersion == 1.72 || VersionInfo.FortniteVersion == 0.00)
				{
					auto& DormantConnections = *(TSet<TWeakObjectPtr<UNetConnection>>*)(__int64(ActorInfo.Get()) + 0x28);

					if (DormantConnections.Contains(Conn))
						continue;
				}
				else if (ActorInfo->DormantConnections.Contains(Conn))
					continue;

				static auto FlushDormancy = FindFlushDormancy();
				if (VersionInfo.FortniteVersion != 1.72 && VersionInfo.FortniteVersion != 0.00 && (VersionInfo.FortniteVersion >= 20 || FlushDormancy))
					if (Actor->GetNetDormancy() > 1 && Channel && !Channel->IsPendingDormancy() && !Channel->IsDormant())
						((int32(*)(UActorChannel*))FindStartBecomingDormant())(Channel);
			}

			bool bIsRelevant = bLevelInitializedForActor && !Actor->bTearOff && (!Channel || Driver->GetTime() - Channel->GetRelevantTime() > 1.0)
				&& IsActorRelevantToConnection(Actor, Viewers);
			bool bIsRecentlyRelevant = bIsRelevant || (Channel && Driver->GetTime() - Channel->GetRelevantTime() < Driver->RelevantTimeout);

			if (bIsRecentlyRelevant && (!Channel || Channel->Actor))
			{
				bAnyRelevant = true;
				auto Priority = 0.f;

				bool bIsAController = false;
				for (auto& Viewer : Viewers)
				{
					if (Actor == Viewer->InViewer)
						bIsAController = true;
				}
				if (Actor->RootComponent && bIsAController)
				{
					FVector Loc;

					GetActorLocation(Actor, FakeStack, &Loc);

					float SmallestDisSquared = (std::numeric_limits<float>::max)();
					int32 ViewersThatSkipActor = 0;

					for (auto& Viewer : Viewers)
					{
						auto DistanceSquared = (Loc - Viewer->ViewLocation).SizeSquared();
						SmallestDisSquared = (float)min(SmallestDisSquared, DistanceSquared);

						if (bDoCullCheck && DistanceSquared > Actor->NetCullDistanceSquared)
							ViewersThatSkipActor++;
					}

					if (bDoCullCheck && ViewersThatSkipActor == Viewers.Num())
						continue;

					const float MaxDistanceScaling = 60000.f * 60000.f;

					const float DistanceFactor = std::clamp((SmallestDisSquared) / MaxDistanceScaling, 0.f, 1.f);

					Priority += DistanceFactor;
				}

				if (Actor->NetDormancy > 1)
					Priority -= 1.5f;

				for (auto& Viewer : Viewers)
					if (Actor == Viewer->ViewTarget || Actor == Viewer->InViewer)
						Priority = -(std::numeric_limits<float>::max)();

				auto& PriorityList = PriorityLists[Conn];
				PriorityList.push_back({ ActorInfo.Get(), Channel, Priority, bIsRelevant, bLevelInitializedForActor });
			}

			if (Channel && !bIsRecentlyRelevant && (Actor->bTearOff || !bLevelInitializedForActor || !(Actor->HasbNetStartup() ? Actor->bNetStartup : false)))
			{
				CloseActorChannel(Channel, Actor->bTearOff ? 4 : 3);
				continue;
			}
		}

		// ActorInfo->NextUpdateTime = TimeSeconds + Scale / Actor->NetUpdateFrequency;

		if (bAnyRelevant)
			((void (*)(AActor*, UNetDriver*))FindCallPreReplication())(Actor, Driver);
	}

	for (auto& PriorityListPair : PriorityLists)
	{
		auto Conn = PriorityListPair.first;
		auto& Viewers = ViewerMap[Conn];
		auto& PriorityActors = PriorityListPair.second;
		int i = 0;

		if (!Conn->ViewTarget)
			goto _out;

		std::sort(PriorityActors.begin(), PriorityActors.end());

		if (IsNetReady && VersionInfo.FortniteVersion < 22 && !IsNetReady(Conn, false))
			goto _out;

		if (DestroyedStartupOrDormantActorGUIDsOffset)
		{
			static auto& DestroyedStartupOrDormantActors = *(TMap<uint32, FActorDestructionInfo*>*)(__int64(Driver) + DestroyedStartupOrDormantActorsOffset);
			static auto& DestroyedStartupOrDormantActors_UE53 = *(TMap<uint64, FActorDestructionInfo*>*)(__int64(Driver) + DestroyedStartupOrDormantActorsOffset);
			auto& DestroyedStartupOrDormantActorGUIDs = *(TSet<uint32>*)(__int64(Conn) + DestroyedStartupOrDormantActorGUIDsOffset);
			auto& DestroyedStartupOrDormantActorGUIDs_UE53 = *(TSet<uint64>*)(__int64(Conn) + DestroyedStartupOrDormantActorGUIDsOffset);
			auto& ClientVisibleLevelNames = *(TSet<int32>*)(__int64(Conn) + ClientVisibleLevelNamesOffset);
			static auto SetChannelActorForDestroy = (void (*)(UActorChannel*, FActorDestructionInfo*))FindSetChannelActorForDestroy();
			static auto SendDestructionInfo = (void (*)(UNetDriver*, UNetConnection*, FActorDestructionInfo*))FindSendDestructionInfo();

			if (VersionInfo.EngineVersion >= 5.3)
			{
				for (auto& NetGUID : DestroyedStartupOrDormantActorGUIDs_UE53)
				{
					auto DestructionInfoPtr = DestroyedStartupOrDormantActors_UE53.Search(
						[&](uint64& GUID, FActorDestructionInfo*& InfoUPtr)
						{
							return GUID
								== NetGUID /* && (InfoUPtr->StreamingLevelName == FName(0) || ClientVisibleLevelNames.Contains(InfoUPtr->StreamingLevelName.ComparisonIndex))*/;
						});

					if (DestructionInfoPtr)
					{
						auto DestructionInfo = *DestructionInfoPtr;

						if (SetChannelActorForDestroy)
						{
							auto Channel = ((UActorChannel * (*)(UNetConnection*, FName*, uint8_t, int)) FindCreateChannel())(Conn, &ActorName, 2, -1);

							if (Channel)
								SetChannelActorForDestroy(Channel, DestructionInfo);
						}
						else if (SendDestructionInfo)
							SendDestructionInfo(Driver, Conn, DestructionInfo);
						// printf("Path: %s\n", DestructionInfo->PathName.ToString().c_str());
					}
				}
				DestroyedStartupOrDormantActorGUIDs_UE53.Reset();
			}
			else
			{
				for (auto& NetGUID : DestroyedStartupOrDormantActorGUIDs)
				{
					auto DestructionInfoPtr = DestroyedStartupOrDormantActors.Search(
						[&](uint32& GUID, FActorDestructionInfo*& InfoUPtr)
						{
							return GUID
								== NetGUID /* && (InfoUPtr->StreamingLevelName == FName(0) || ClientVisibleLevelNames.Contains(InfoUPtr->StreamingLevelName.ComparisonIndex))*/;
						});

					if (DestructionInfoPtr)
					{
						auto DestructionInfo = *DestructionInfoPtr;

						if (SetChannelActorForDestroy)
						{
							auto Channel = ((UActorChannel * (*)(UNetConnection*, FName*, uint8_t, int)) FindCreateChannel())(Conn, &ActorName, 2, -1);

							if (Channel)
								SetChannelActorForDestroy(Channel, DestructionInfo);
						}
						else if (SendDestructionInfo)
							SendDestructionInfo(Driver, Conn, DestructionInfo);
						// printf("Path: %s\n", DestructionInfo->PathName.ToString().c_str());
					}
				}
				DestroyedStartupOrDormantActorGUIDs.Reset();
			}
		}

		static auto SendClientAdjustment = FindSendClientAdjustment();
		if (SendClientAdjustment)
			for (auto& Viewer : Viewers)
			{
				if (Viewer->Connection->PlayerController)
					((void (*)(AFortPlayerControllerAthena*))SendClientAdjustment)(Viewer->Connection->PlayerController);
			}

		for (auto& PriorityActor : PriorityActors)
		{
			auto ActorInfo = PriorityActor.ActorInfo;

			UActorChannel* Channel = PriorityActor.Channel;

			if (!Channel || Channel->Actor)
			{
				auto Actor = ActorInfo->Actor;

				if (!Channel)
				{
					if (VersionInfo.FortniteVersion >= 20)
						Channel = ((UActorChannel * (*)(UNetConnection*, FName*, uint8_t, int)) FindCreateChannel())(Conn, &ActorName, 2, -1);
					else
						Channel = ((UActorChannel * (*)(UNetConnection*, int, bool, int32_t)) FindCreateChannel())(Conn, 2, true, -1);

					if (Channel)
						((void (*)(UActorChannel*, AActor*, uint8_t))FindSetChannelActor())(Channel, Actor, 0);
				}

				if (Channel)
				{
					if (PriorityActor.bIsRelevant)
						Channel->GetRelevantTime() = Driver->GetTime() + 0.5 * ((float)rand() / 32767.f);

					if (VersionInfo.FortniteVersion >= 22 || (IsNetReady && IsNetReady(Conn, false))) // actually uchannel::isnetready
						((int32(*)(UActorChannel*))FindReplicateActor())(Channel);
					else
						Actor->ForceNetUpdate();

					if (IsNetReady && VersionInfo.FortniteVersion < 22 && !IsNetReady(Conn, false))
					{
						break;
					}
				}

				if (Channel && Actor->bTearOff && (!PriorityActor.bLevelInitializedForActor || !(Actor->HasbNetStartup() ? Actor->bNetStartup : false)))
					CloseActorChannel(Channel, 4);
			}
			i++;
		}

	_out:
		PriorityActors.clear();
	}
	PriorityLists.clear();

	for (auto& ViewerPair : ViewerMap)
	{
		for (auto& Viewer : ViewerPair.second)
			free(Viewer);

		ViewerPair.second.Free();
	}

	ViewerMap.clear();
}

static bool GetActiveStormEffectHandle(UAbilitySystemComponent* AbilitySystemComponent, UClass* StormEffectClass, FActiveGameplayEffectHandle& OutHandle)
{
	if (!AbilitySystemComponent || !StormEffectClass)
		return false;

	auto& Effects = AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal;

	for (int i = 0; i < Effects.Num(); i++)
	{
		auto& Effect = Effects.Get(i, FActiveGameplayEffect::Size());

		if (Effect.Spec.Def && Effect.Spec.Def->IsA(StormEffectClass))
		{
			OutHandle = *(FActiveGameplayEffectHandle*)(__int64(&Effect) + 0xc);
			return true;
		}
	}

	return false;
}

static void RemoveStormEffect(UAbilitySystemComponent* AbilitySystemComponent, UClass* StormEffectClass)
{
	if (!AbilitySystemComponent || !StormEffectClass)
		return;

	static auto RemoveActiveGameplayEffectBySourceEffectFn = const_cast<UFunction*>(FindObject<UFunction>(L"/Script/GameplayAbilities.AbilitySystemComponent.RemoveActiveGameplayEffectBySourceEffect"));
	if (!RemoveActiveGameplayEffectBySourceEffectFn)
		return;

	struct
	{
		UClass* GameplayEffect;
		UObject* InstigatorAbilitySystemComponent;
		int StacksToRemove;
	} Params{ StormEffectClass, AbilitySystemComponent, 1 };

	AbilitySystemComponent->ProcessEvent(RemoveActiveGameplayEffectBySourceEffectFn, &Params);
}

static bool TrySetReflectedBool(UObject* Object, const char* PropertyName, bool Value, bool& bOutChanged)
{
	if (!Object)
		return false;

	auto Property = Object->GetProperty(PropertyName, 0x20000);
	if (!Property)
		return false;

	auto Offset = GetFromOffset<uint32>(Property, Offsets::Offset_Internal);
	auto FieldMask = Property->GetFieldMask();
	auto& ByteValue = GetFromOffset<uint8_t>(Object, Offset);
	bool bPreviousValue = FieldMask != 0 ? (ByteValue & FieldMask) != 0 : ByteValue != 0;

	if (FieldMask != 0)
	{
		if (Value)
			ByteValue |= FieldMask;
		else
			ByteValue &= ~FieldMask;
	}
	else
	{
		ByteValue = Value ? 1 : 0;
	}

	bOutChanged = bPreviousValue != Value;
	return true;
}

static void TryCallZeroedFunction(UObject* Object, const char* FunctionName)
{
	if (!Object)
		return;

	auto Function = Object->GetFunction(FunctionName);
	if (!Function)
		return;

	auto ParamsSize = Function->GetPropertiesSize();
	if (ParamsSize <= 0)
	{
		Object->ProcessEvent(Function, nullptr);
		return;
	}

	auto Params = FMemory::Malloc(ParamsSize);
	memset((PBYTE)Params, 0, ParamsSize);
	Object->ProcessEvent(Function, Params);
	FMemory::Free(Params);
}

static void SyncReflectedStormState(AFortPlayerPawnAthena* Pawn, bool bInZone)
{
	if (!Pawn)
		return;

	bool bAnyStormPropertyFound = false;
	bool bAnyStormPropertyChanged = false;

	struct FStormBoolProperty
	{
		const char* Name;
		bool Value;
	};

	const FStormBoolProperty StormProperties[] =
	{
		{ "bIsInsideSafeZone", bInZone },
		{ "bIsInSafeZone", bInZone },
		{ "bIsOutsideSafeZone", !bInZone },
		{ "bIsInAnyStorm", !bInZone },
		{ "bIsInStorm", !bInZone },
		{ "bInStorm", !bInZone }
	};

	for (auto& StormProperty : StormProperties)
	{
		bool bChanged = false;
		if (TrySetReflectedBool(Pawn, StormProperty.Name, StormProperty.Value, bChanged))
		{
			bAnyStormPropertyFound = true;
			bAnyStormPropertyChanged |= bChanged;
		}
	}

	if (!bAnyStormPropertyChanged)
		return;

	static const char* StormOnRepFunctions[] =
	{
		"OnRep_IsInsideSafeZone",
		"OnRep_bIsInsideSafeZone",
		"OnRep_IsInSafeZone",
		"OnRep_bIsInSafeZone",
		"OnRep_IsOutsideSafeZone",
		"OnRep_bIsOutsideSafeZone",
		"OnRep_IsInAnyStorm",
		"OnRep_bIsInAnyStorm",
		"OnRep_IsInStorm",
		"OnRep_bIsInStorm",
		"OnRep_InStorm"
	};

	for (auto OnRepFunction : StormOnRepFunctions)
		TryCallZeroedFunction(Pawn, OnRepFunction);

	if (bAnyStormPropertyFound)
		Pawn->ForceNetUpdate();
}

static void SetSafeZoneAppliedGameplayEffectHandle(AFortPlayerPawnAthena* Pawn, const FActiveGameplayEffectHandle* StormEffectHandle)
{
	if (!Pawn)
		return;

	auto Property = Pawn->GetProperty("SafeZoneAppliedGE");
	if (!Property)
		return;

	auto ElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
	if (ElementSize < sizeof(int32) || ElementSize > 0x20)
		return;

	auto Offset = GetFromOffset<uint32>(Property, Offsets::Offset_Internal);
	auto Destination = (PBYTE)(__int64(Pawn) + Offset);
	memset(Destination, 0, ElementSize);

	if (StormEffectHandle)
		memcpy(Destination, StormEffectHandle, (std::min)(ElementSize, (uint32)sizeof(FActiveGameplayEffectHandle)));

	TryCallZeroedFunction(Pawn, "OnRep_SafeZoneAppliedGE");
	Pawn->ForceNetUpdate();
}

static bool IsFiniteSafeZoneVector(FVector Location)
{
	return std::isfinite(static_cast<double>(Location.X)) &&
		std::isfinite(static_cast<double>(Location.Y)) &&
		std::isfinite(static_cast<double>(Location.Z));
}

static bool IsUsableSafeZoneRadius(float Radius, bool bAllowZero = false)
{
	return (bAllowZero ? Radius >= 0.f : Radius > 0.f) && std::isfinite(static_cast<double>(Radius));
}

static bool TryUseSafeZoneCircle(FVector Center, float Radius, FVector& OutCenter, float& OutRadius, bool bAllowZeroCenter, bool bAllowZeroRadius = false)
{
	if (!IsUsableSafeZoneRadius(Radius, bAllowZeroRadius) || !IsFiniteSafeZoneVector(Center) || (!bAllowZeroCenter && Center.IsZero()))
		return false;

	OutCenter = Center;
	OutRadius = Radius;
	return true;
}

static bool TryGetCurrentPhaseSafeZoneCircle(AFortSafeZoneIndicator* SafeZoneIndicator, FVector& OutCenter, float& OutRadius)
{
	if (!SafeZoneIndicator || !SafeZoneIndicator->HasSafeZonePhases() || !SafeZoneIndicator->HasCurrentPhase())
		return false;

	if (!FFortSafeZonePhaseInfo::HasCenter() || !FFortSafeZonePhaseInfo::HasRadius())
		return false;

	auto& SafeZonePhases = SafeZoneIndicator->SafeZonePhases;
	auto CurrentPhase = SafeZoneIndicator->CurrentPhase;

	if (!SafeZonePhases.IsValidIndex(CurrentPhase))
		return false;

	auto& PhaseInfo = SafeZonePhases.Get(CurrentPhase, FFortSafeZonePhaseInfo::Size());
	return TryUseSafeZoneCircle(PhaseInfo.Center, PhaseInfo.Radius, OutCenter, OutRadius, true, true);
}

static bool TryGetLiveSafeZoneCenter(AFortSafeZoneIndicator* SafeZoneIndicator, FVector& OutCenter, bool bAllowZeroCenter)
{
	if (!SafeZoneIndicator)
		return false;

	if (auto GetSafeZoneCenterFn = SafeZoneIndicator->GetFunction("GetSafeZoneCenter"))
	{
		auto Center = SafeZoneIndicator->Call<FVector>(GetSafeZoneCenterFn);

		if (IsFiniteSafeZoneVector(Center) && (bAllowZeroCenter || !Center.IsZero()))
		{
			OutCenter = Center;
			return true;
		}
	}

	return false;
}

static bool TryGetInterpolatedSafeZoneCircle(AFortSafeZoneIndicator* SafeZoneIndicator, FVector& OutCenter, float& OutRadius)
{
	if (!SafeZoneIndicator ||
		!SafeZoneIndicator->HasPreviousCenter() || !SafeZoneIndicator->HasPreviousRadius() ||
		!SafeZoneIndicator->HasNextCenter() || !SafeZoneIndicator->HasNextRadius() ||
		!SafeZoneIndicator->HasSafeZoneStartShrinkTime() || !SafeZoneIndicator->HasSafeZoneFinishShrinkTime())
	{
		return false;
	}

	auto PreviousCenter = SafeZoneIndicator->PreviousCenter;
	auto NextCenter = SafeZoneIndicator->NextCenter;
	auto PreviousRadius = SafeZoneIndicator->PreviousRadius;
	auto NextRadius = SafeZoneIndicator->NextRadius;

	if (!IsFiniteSafeZoneVector(PreviousCenter) || !IsFiniteSafeZoneVector(NextCenter) ||
		!IsUsableSafeZoneRadius(PreviousRadius) || !IsUsableSafeZoneRadius(NextRadius, true))
	{
		return false;
	}

	auto StartShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime;
	auto FinishShrinkTime = SafeZoneIndicator->SafeZoneFinishShrinkTime;
	auto Alpha = 1.f;

	if (FinishShrinkTime > StartShrinkTime)
	{
		auto TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
		Alpha = (TimeSeconds - StartShrinkTime) / (FinishShrinkTime - StartShrinkTime);

		if (Alpha < 0.f)
			Alpha = 0.f;
		else if (Alpha > 1.f)
			Alpha = 1.f;
	}

	auto Center = PreviousCenter + ((NextCenter - PreviousCenter) * Alpha);
	auto Radius = PreviousRadius + ((NextRadius - PreviousRadius) * Alpha);

	return TryUseSafeZoneCircle(Center, Radius, OutCenter, OutRadius, true, true);
}

static bool TryGetSafeZoneCircle(AFortSafeZoneIndicator* SafeZoneIndicator, FVector& OutCenter, float& OutRadius)
{
	if (!SafeZoneIndicator)
		return false;

	FVector PhaseCenter{};
	float PhaseRadius = 0.f;
	const bool bHasPhaseCircle = TryGetCurrentPhaseSafeZoneCircle(SafeZoneIndicator, PhaseCenter, PhaseRadius);

	FVector LiveCenter{};
	const bool bHasLiveCenter = TryGetLiveSafeZoneCenter(SafeZoneIndicator, LiveCenter, bHasPhaseCircle);

	if (TryGetInterpolatedSafeZoneCircle(SafeZoneIndicator, OutCenter, OutRadius))
		return true;

	if (SafeZoneIndicator->HasRadius() && IsUsableSafeZoneRadius(SafeZoneIndicator->Radius))
	{
		if (bHasLiveCenter && TryUseSafeZoneCircle(LiveCenter, SafeZoneIndicator->Radius, OutCenter, OutRadius, true))
			return true;

		if (bHasPhaseCircle && TryUseSafeZoneCircle(PhaseCenter, SafeZoneIndicator->Radius, OutCenter, OutRadius, true))
			return true;

		if (SafeZoneIndicator->HasNextCenter() && TryUseSafeZoneCircle(SafeZoneIndicator->NextCenter, SafeZoneIndicator->Radius, OutCenter, OutRadius, false))
			return true;

		if (SafeZoneIndicator->HasLastCenter() && TryUseSafeZoneCircle(SafeZoneIndicator->LastCenter, SafeZoneIndicator->Radius, OutCenter, OutRadius, false))
			return true;

		if (SafeZoneIndicator->HasPreviousCenter() && TryUseSafeZoneCircle(SafeZoneIndicator->PreviousCenter, SafeZoneIndicator->Radius, OutCenter, OutRadius, false))
			return true;
	}

	if (bHasPhaseCircle)
	{
		OutCenter = PhaseCenter;
		OutRadius = PhaseRadius;
		return true;
	}

	if (SafeZoneIndicator->HasNextCenter() && SafeZoneIndicator->HasNextRadius() &&
		TryUseSafeZoneCircle(SafeZoneIndicator->NextCenter, SafeZoneIndicator->NextRadius, OutCenter, OutRadius, false, true))
	{
		return true;
	}

	if (SafeZoneIndicator->HasLastCenter() && SafeZoneIndicator->HasLastRadius() &&
		TryUseSafeZoneCircle(SafeZoneIndicator->LastCenter, SafeZoneIndicator->LastRadius, OutCenter, OutRadius, false))
	{
		return true;
	}

	if (SafeZoneIndicator->HasPreviousCenter() && SafeZoneIndicator->HasPreviousRadius() &&
		TryUseSafeZoneCircle(SafeZoneIndicator->PreviousCenter, SafeZoneIndicator->PreviousRadius, OutCenter, OutRadius, false))
	{
		return true;
	}

	return false;
}

static bool IsLocationInsideSafeZoneCircle(FVector Location, FVector Center, float Radius)
{
	auto DeltaX = static_cast<double>(Center.X - Location.X);
	auto DeltaY = static_cast<double>(Center.Y - Location.Y);
	auto RadiusDouble = static_cast<double>(Radius);

	return (DeltaX * DeltaX) + (DeltaY * DeltaY) <= RadiusDouble * RadiusDouble;
}

static float GetDefaultLowerSeasonStormDamagePerSecond(int Phase)
{
	constexpr float DamageByPhase[] = { 1.f, 1.f, 1.f, 2.f, 5.f, 7.5f, 10.f, 10.f, 10.f, 10.f, 10.f, 10.f, 10.f };

	if (Phase < 0)
		Phase = 0;

	const int LastIndex = (int)(sizeof(DamageByPhase) / sizeof(DamageByPhase[0])) - 1;
	return DamageByPhase[Phase > LastIndex ? LastIndex : Phase];
}

static bool ShouldUseLowerSeasonStormFixes()
{
	return VersionInfo.FortniteVersion < 7.00;
}

static bool ShouldSyncStormState()
{
	return ShouldUseLowerSeasonStormFixes() || (VersionInfo.FortniteVersion >= 18.00 && VersionInfo.FortniteVersion < 25.20);
}

static int GetSafeZoneDamagePhase(AFortGameMode* GameMode, AFortSafeZoneIndicator* SafeZoneIndicator)
{
	if (GameMode && GameMode->HasSafeZonePhase() && GameMode->SafeZonePhase > 0)
		return GameMode->SafeZonePhase;

	if (SafeZoneIndicator && SafeZoneIndicator->HasCurrentPhase() && SafeZoneIndicator->CurrentPhase > 0)
		return SafeZoneIndicator->CurrentPhase;

	return 1;
}

static float GetStormDamagePerTick(AFortPlayerPawnAthena* Pawn, AFortSafeZoneIndicator* SafeZoneIndicator, int StormPhase)
{
	float DefaultDamage = 1.f;
	if (ShouldUseLowerSeasonStormFixes())
		DefaultDamage = GetDefaultLowerSeasonStormDamagePerSecond(StormPhase);

	if (ShouldUseLowerSeasonStormFixes())
		return DefaultDamage;

	if (!SafeZoneIndicator || !SafeZoneIndicator->HasCurrentDamageInfo() || !FFortSafeZoneDamageInfo::HasDamage())
		return DefaultDamage;

	auto Damage = SafeZoneIndicator->CurrentDamageInfo.Damage;

	if (!std::isfinite(static_cast<double>(Damage)) || Damage <= 0.f)
		return DefaultDamage;

	if (FFortSafeZoneDamageInfo::HasbPercentageBasedDamage() && SafeZoneIndicator->CurrentDamageInfo.bPercentageBasedDamage)
	{
		auto MaxHealth = Pawn ? Pawn->GetMaxHealth() : 100.f;

		if (!std::isfinite(static_cast<double>(MaxHealth)) || MaxHealth <= 0.f)
			MaxHealth = 100.f;

		Damage *= MaxHealth;
	}

	return Damage < 1.f ? 1.f : Damage;
}

static void InvokeStormDamageGameplayCue(UAbilitySystemComponent* AbilitySystemComponent, AFortPlayerControllerAthena* Player, AFortPlayerPawnAthena* Pawn, float Damage)
{
	if (!AbilitySystemComponent)
		return;

	FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();
	Context.Instigator = Player;
	Context.Causer = Pawn;
	Context.AddSourceObject(Pawn);

	auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
	memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());

	const wchar_t* StormDamageCueNames[] =
	{
		L"GameplayCue.Athena.Storm.Damage",
		L"GameplayCue.Athena.SafeZone.Damage",
		L"GameplayCue.Athena.OutsideSafeZone.Damage",
		L"GameplayCue.Athena.StormDamage",
		L"GameplayCue.SafeZone.Damage",
		L"GameplayCue.Storm.Damage"
	};

	for (auto CueName : StormDamageCueNames)
	{
		FGameplayTag CueTag{};
		CueTag.TagName = FName(CueName);
		AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(CueTag, *PredictionKey, Context);

		auto CueParamsStruct = FGameplayCueParameters::StaticStruct();
		if (CueParamsStruct)
		{
			FGameplayCueParameters Params{};

			if (FGameplayCueParameters::HasEffectContext())
				Params.EffectContext = Context;

			auto RawMagnitudeProperty = FGameplayCueParameters::StaticStruct()->GetProperty("RawMagnitude");
			if (RawMagnitudeProperty)
			{
				auto Offset = GetFromOffset<uint32>(RawMagnitudeProperty, Offsets::Offset_Internal);
				if (Offset + sizeof(float) <= sizeof(FGameplayCueParameters))
					GetFromOffset<float>(&Params, Offset) = Damage;
			}

			auto NormalizedMagnitudeProperty = FGameplayCueParameters::StaticStruct()->GetProperty("NormalizedMagnitude");
			if (NormalizedMagnitudeProperty)
			{
				auto Offset = GetFromOffset<uint32>(NormalizedMagnitudeProperty, Offsets::Offset_Internal);
				if (Offset + sizeof(float) <= sizeof(FGameplayCueParameters))
					GetFromOffset<float>(&Params, Offset) = Damage;
			}

			auto LocationProperty = FGameplayCueParameters::StaticStruct()->GetProperty("Location");
			if (LocationProperty && Pawn)
			{
				auto Offset = GetFromOffset<uint32>(LocationProperty, Offsets::Offset_Internal);
				if (Offset + sizeof(FVector) <= sizeof(FGameplayCueParameters))
					GetFromOffset<FVector>(&Params, Offset) = Pawn->K2_GetActorLocation();
			}

			AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted_WithParams(CueTag, *PredictionKey, Params);
		}
	}

	free(PredictionKey);
}

static void TryForceKillPawnForStorm(AFortPlayerPawnAthena* Pawn)
{
	if (!Pawn)
		return;

	auto ForceKillFn = Pawn->GetFunction("ForceKill");

	if (ForceKillFn)
	{
		FGameplayTag DeathTag{};
		AFortPlayerControllerAthena* InstigatorController = nullptr;
		AActor* DamageCauser = nullptr;
		Pawn->Call<void>(ForceKillFn, DeathTag, InstigatorController, DamageCauser);
		return;
	}

	Pawn->SetHealth(0.f);
	Pawn->ForceNetUpdate();
}

static std::unordered_map<AFortPlayerPawnAthena*, float> LowerSeasonStormDamageTimes;

static void ResetLowerSeasonStormDamageDelay(AFortPlayerPawnAthena* Pawn)
{
	if (Pawn)
		LowerSeasonStormDamageTimes.erase(Pawn);
}

static void ApplyLowerSeasonStormDamage(AFortPlayerControllerAthena* Player, AFortPlayerPawnAthena* Pawn, float TimeSeconds, UAbilitySystemComponent* AbilitySystemComponent, float Damage)
{
	if (!Pawn)
		return;

	if (Pawn->HasbCanBeDamaged() && !Pawn->bCanBeDamaged)
		return;

	auto LastDamageTimeIt = LowerSeasonStormDamageTimes.find(Pawn);
	if (LastDamageTimeIt == LowerSeasonStormDamageTimes.end())
	{
		LowerSeasonStormDamageTimes[Pawn] = TimeSeconds;
		return;
	}

	if (TimeSeconds - LastDamageTimeIt->second < 1.f)
		return;

	LastDamageTimeIt->second = TimeSeconds;

	auto Health = Pawn->GetHealth();
	const bool bFatal = Health <= Damage;

	if (bFatal)
		TryForceKillPawnForStorm(Pawn);
	else
		Pawn->SetHealth(Health - Damage);

	InvokeStormDamageGameplayCue(AbilitySystemComponent, Player, Pawn, Damage);
	Pawn->ForceNetUpdate();
}

static bool IsStormIgnoredBot(AFortPlayerControllerAthena* Player)
{
	if (!Player || !Player->PlayerState)
		return false;

	auto PlayerState = (AFortPlayerStateAthena*)Player->PlayerState;
	return PlayerState->HasbIsABot() && PlayerState->bIsABot;
}

static void SyncStormState(AFortGameMode* GameMode)
{
	if (!GameMode || !GameMode->SafeZoneIndicator)
		return;

	const bool bUseLowerSeasonStormFixes = ShouldUseLowerSeasonStormFixes();
	if (!bUseLowerSeasonStormFixes && !ShouldSyncStormState())
		return;

	static auto StormEffectClass = const_cast<UClass*>(FindObject<UClass>(L"/Game/Athena/SafeZone/GE_OutsideSafeZoneDamage.GE_OutsideSafeZoneDamage_C"));
	static std::unordered_map<AFortPlayerPawnAthena*, bool> LastKnownSafeZoneStates;
	const float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

	for (auto& UncastedPlayer : GameMode->AlivePlayers)
	{
		auto Player = (AFortPlayerControllerAthena*)UncastedPlayer;
		if (!Player)
			continue;

		if (IsStormIgnoredBot(Player))
			continue;

		auto Pawn = Player->MyFortPawn;
		if (!Pawn)
			Pawn = Player->Pawn;

		if (!Pawn || !Pawn->IsA(AFortPlayerPawnAthena::StaticClass()))
			continue;

		auto PawnLocation = Pawn->K2_GetActorLocation();
		bool bInZone = true;

		if (bUseLowerSeasonStormFixes)
		{
			FVector SafeZoneCenter{};
			float SafeZoneRadius = 0.f;

			if (TryGetSafeZoneCircle(GameMode->SafeZoneIndicator, SafeZoneCenter, SafeZoneRadius))
				bInZone = IsLocationInsideSafeZoneCircle(PawnLocation, SafeZoneCenter, SafeZoneRadius);
			else
				bInZone = GameMode->IsInCurrentSafeZone(PawnLocation, false);
		}
		else
		{
			bInZone = GameMode->IsInCurrentSafeZone(PawnLocation, false);
		}

		auto LastKnownSafeZoneState = LastKnownSafeZoneStates.find(Pawn);

		if (LastKnownSafeZoneState == LastKnownSafeZoneStates.end() || LastKnownSafeZoneState->second != bInZone)
		{
			printf("Pawn %s new storm status: %s\n", Pawn->Name.ToString().c_str(), bInZone ? "true" : "false");
			LastKnownSafeZoneStates[Pawn] = bInZone;
		}

		SyncReflectedStormState(Pawn, bInZone);

		if (!bUseLowerSeasonStormFixes)
			continue;

		const int StormPhase = GetSafeZoneDamagePhase(GameMode, GameMode->SafeZoneIndicator);
		const float StormDamage = GetStormDamagePerTick(Pawn, GameMode->SafeZoneIndicator, StormPhase);

		if (bInZone)
			ResetLowerSeasonStormDamageDelay(Pawn);

		if (!StormEffectClass || !Player->PlayerState)
		{
			if (!bInZone)
				ApplyLowerSeasonStormDamage(Player, Pawn, TimeSeconds, nullptr, StormDamage);

			continue;
		}

		auto AbilitySystemComponent = Player->PlayerState->AbilitySystemComponent;
		if (!AbilitySystemComponent)
		{
			if (!bInZone)
				ApplyLowerSeasonStormDamage(Player, Pawn, TimeSeconds, nullptr, StormDamage);

			continue;
		}

		FActiveGameplayEffectHandle StormEffectHandle{};
		bool bHasStormEffect = GetActiveStormEffectHandle(AbilitySystemComponent, StormEffectClass, StormEffectHandle);

		if (bInZone)
		{
			if (bHasStormEffect)
				RemoveStormEffect(AbilitySystemComponent, StormEffectClass);

			SetSafeZoneAppliedGameplayEffectHandle(Pawn, nullptr);
			continue;
		}

		if (!bHasStormEffect)
		{
			FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();
			Context.Instigator = Player;
			Context.Causer = Pawn;
			Context.AddSourceObject(Pawn);

			StormEffectHandle = AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(StormEffectClass, (float)StormPhase, Context);
			bHasStormEffect = StormEffectHandle.Handle != 0;
		}

		if (bHasStormEffect)
		{
			SetSafeZoneAppliedGameplayEffectHandle(Pawn, &StormEffectHandle);

			AbilitySystemComponent->SetActiveGameplayEffectLevel(StormEffectHandle, StormPhase);
			AbilitySystemComponent->UpdateActiveGameplayEffectSetByCallerMagnitude(StormEffectHandle,
				FGameplayTag(FName(L"SetByCaller.StormCampingDamage")), StormDamage);
			AbilitySystemComponent->UpdateActiveGameplayEffectSetByCallerMagnitude(StormEffectHandle,
				FGameplayTag(FName(L"SetByCaller.StormShieldDamage")), StormDamage);
		}

		ApplyLowerSeasonStormDamage(Player, Pawn, TimeSeconds, AbilitySystemComponent, StormDamage);
	}
}

void UNetDriver::TickFlush(UNetDriver* Driver, float DeltaSeconds)
{
	if (VersionInfo.FortniteVersion >= 25.20)
	{
		auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());
		GamePhaseLogic->Tick();
	}

	AFortPlayerControllerAthena::TickNukeRockets(DeltaSeconds);

	// Universal PlayerAI system (separate from the bot command and native AI).
	MagnesiumPlayerAIIntegration::OnServerTick(Driver, DeltaSeconds);

	if (Driver->ClientConnections.Num() > 0)
		ServerReplicateActors(Driver, DeltaSeconds);

    if (GUI::gsStatus == Joinable && VersionInfo.FortniteVersion >= 11.00)
    { 
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
		auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
		static auto bSkipAircraft = GameState->HasCurrentPlaylistInfo() && GameState->CurrentPlaylistInfo.BasePlaylist ? GameState->CurrentPlaylistInfo.BasePlaylist->bSkipAircraft : false;
		auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
		if (!bSkipAircraft && GameState->HasWarmupCountdownEndTime() && GameMode->MatchState == FName(L"InProgress") && GameState->WarmupCountdownEndTime <= Time)
        {
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startaircraft"), nullptr);
        }
	}
	else if (GUI::gsStatus == StartedMatch && (FConfiguration::bAutoRestart || (FConfiguration::WebhookURL && *FConfiguration::WebhookURL) || ShouldSyncStormState()))
	{
		auto WorldNetDriver = UWorld::GetWorld()->NetDriver;
		auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
		if (Driver == WorldNetDriver && Driver->ClientConnections.Num() == 0)
		{
			static bool stopped = false;

			if (!stopped)
			{
				stopped = true;

				if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
				{
					auto curl = curl_easy_init();

					curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
					curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
					curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

					char version[6];

					sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);

    				auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;
					auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Match has ended!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" + (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"}], \"color\": " + "\"7237230\", \"footer\": {\"text\":\"Erbium\", \"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" + iso8601() + "\"}] }";

					curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

					curl_easy_perform(curl);

					curl_easy_cleanup(curl);
				}

				if (FConfiguration::bAutoRestart)
					TerminateProcess(GetCurrentProcess(), 0);
			}
		}
		else if (Driver == WorldNetDriver && ShouldSyncStormState())
		{
			SyncStormState(GameMode);
		}
	}

    return TickFlushOG(Driver, DeltaSeconds);
}

uint64_t ServerReplicateActors_;
void UNetDriver::TickFlush__RepGraph(UNetDriver* Driver, float DeltaSeconds)
{
	// Universal PlayerAI system (separate from the bot command and native AI).
	MagnesiumPlayerAIIntegration::OnServerTick(Driver, DeltaSeconds);

	if (Driver->ReplicationDriver)
	{
		AFortPlayerControllerAthena::TickNukeRockets(DeltaSeconds);

		// this is our main netdriver
		if (Driver->ClientConnections.Num() > 0)
			((void (*)(UObject*, float)) ServerReplicateActors_)(Driver->ReplicationDriver, DeltaSeconds);


		if (GUI::gsStatus == Joinable && VersionInfo.FortniteVersion >= 11.00)
		{
			auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
			auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
			auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
			static auto bSkipAircraft = GameState->HasCurrentPlaylistInfo() && GameState->CurrentPlaylistInfo.BasePlaylist ? GameState->CurrentPlaylistInfo.BasePlaylist->bSkipAircraft : false;
			if (!bSkipAircraft && GameState->HasWarmupCountdownEndTime() && GameMode->MatchState == FName(L"InProgress") && GameState->WarmupCountdownEndTime <= Time)
			{
				UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startaircraft"), nullptr);
			}
		}
		else if (GUI::gsStatus == StartedMatch && (FConfiguration::bAutoRestart || (FConfiguration::WebhookURL && *FConfiguration::WebhookURL) || ShouldSyncStormState()))
		{
			auto WorldNetDriver = UWorld::GetWorld()->NetDriver;
			auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
			if (Driver == WorldNetDriver && Driver->ClientConnections.Num() == 0)
			{
				static bool stopped = false;

				if (!stopped)
				{
					stopped = true;

					if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
					{
						auto curl = curl_easy_init();

						curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
						curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
						curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

						char version[6];

						sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);

        				auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;
						auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Match has ended!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" + (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"}], \"color\": " + "\"7237230\", \"footer\": {\"text\":\"Erbium\", \"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" + iso8601() + "\"}] }";

						curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

						curl_easy_perform(curl);

						curl_easy_cleanup(curl);
					}

					if (FConfiguration::bAutoRestart)
						TerminateProcess(GetCurrentProcess(), 0);
				}
			}
			else if (Driver == WorldNetDriver && ShouldSyncStormState())
			{
				SyncStormState(GameMode);
			}
		}
	}

	return TickFlushOG(Driver, DeltaSeconds);
}


enum class EReplicationSystemSendPass : unsigned
{
	Invalid,
	PostTickDispatch,
	TickFlush,
};

struct FSendUpdateParams
{
	EReplicationSystemSendPass SendPass = EReplicationSystemSendPass::TickFlush;
	float DeltaSeconds = 0.f;
};

void SendClientMoveAdjustments(UNetDriver* Driver)
{
	static auto SendClientAdjustment = (void(*)(AFortPlayerControllerAthena*)) FindSendClientAdjustment();
	if (SendClientAdjustment)
	{
		for (UNetConnection* Connection : Driver->ClientConnections)
		{
			if (Connection == nullptr || Connection->ViewTarget == nullptr)
				continue;

			if (AFortPlayerControllerAthena* PC = Connection->PlayerController)
				SendClientAdjustment(PC);

			for (UNetConnection* ChildConnection : Connection->Children)
			{
				if (ChildConnection == nullptr)
					continue;

				if (AFortPlayerControllerAthena* PC = ChildConnection->PlayerController)
					SendClientAdjustment(PC);
			}
		}
	}
}

void UNetDriver::TickFlush__Iris(UNetDriver* Driver, float DeltaSeconds)
{
	if (VersionInfo.FortniteVersion >= 25.20)
	{
		auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());
		if (GamePhaseLogic)
			GamePhaseLogic->Tick();
	}

	AFortPlayerControllerAthena::TickNukeRockets(DeltaSeconds);

	// Universal PlayerAI system (separate from the bot command and native AI).
	MagnesiumPlayerAIIntegration::OnServerTick(Driver, DeltaSeconds);

	if (Driver->ClientConnections.Num() > 0)
	{
		auto ReplicationSystem = *(UObject**)(__int64(&Driver->ReplicationDriver) + 8);

		if (ReplicationSystem)
		{
			static void(*UpdateIrisReplicationViews)(UNetDriver*) = decltype(UpdateIrisReplicationViews)(FindUpdateIrisReplicationViews());
			static void(*PreSendUpdate)(UObject*, FSendUpdateParams&) = decltype(PreSendUpdate)(FindPreSendUpdate());

			UpdateIrisReplicationViews(Driver);
			SendClientMoveAdjustments(Driver);
			FSendUpdateParams Params;
			Params.DeltaSeconds = DeltaSeconds;
			PreSendUpdate(ReplicationSystem, Params);
		}
	}

	if (GUI::gsStatus == Joinable && VersionInfo.FortniteVersion < 25.20)
	{
		auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
		auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
		auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
		static auto bSkipAircraft = GameState->HasCurrentPlaylistInfo() && GameState->CurrentPlaylistInfo.BasePlaylist ? GameState->CurrentPlaylistInfo.BasePlaylist->bSkipAircraft : false;
		if (!bSkipAircraft && GameState->HasWarmupCountdownEndTime() && GameMode->MatchState == FName(L"InProgress") && GameState->WarmupCountdownEndTime <= Time)
		{
			UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startaircraft"), nullptr);
		}
	}
	else if (GUI::gsStatus == StartedMatch && (FConfiguration::bAutoRestart || (FConfiguration::WebhookURL && *FConfiguration::WebhookURL) || ShouldSyncStormState()))
	{
		auto WorldNetDriver = UWorld::GetWorld()->NetDriver;
		auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
		if (Driver == WorldNetDriver && Driver->ClientConnections.Num() == 0)
		{
			static bool stopped = false;

			if (!stopped)
			{
				stopped = true;

				if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
				{
					auto curl = curl_easy_init();

					curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
					curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
					curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

					char version[6];

					sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);

					
        			auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;
					auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Match has ended!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" + (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"}], \"color\": " + "\"7237230\", \"footer\": {\"text\":\"Erbium\", \"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" + iso8601() + "\"}] }";

					curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

					curl_easy_perform(curl);

					curl_easy_cleanup(curl);
				}

				if (FConfiguration::bAutoRestart)
					TerminateProcess(GetCurrentProcess(), 0);
			}
		}
		else if (Driver == WorldNetDriver && ShouldSyncStormState())
		{
			SyncStormState(GameMode);
		}
	}

	return TickFlushOG(Driver, DeltaSeconds);
}

void (*SetNetDormancyOG)(AActor* Actor, int NewDormancy);
void SetNetDormancy(AActor* Actor, int NewDormancy)
{
	auto Driver = (UNetDriver*)UWorld::GetWorld()->NetDriver;


	SetNetDormancyOG(Actor, NewDormancy);

	if (Driver)
		if (NewDormancy <= 1)
			for (auto& Conn : Driver->ClientConnections)
				((void(*)(UNetConnection*, AActor*)) FindFlushDormancy())(Conn, Actor);
}

void (*FlushNetDormancyOG)(AActor* Actor);
void FlushNetDormancy(AActor* Actor)
{
	auto Driver = (UNetDriver*)UWorld::GetWorld()->NetDriver;

	FlushNetDormancyOG(Actor);

	if (Driver)
		if (Actor->NetDormancy > 1)
			for (auto& Conn : Driver->ClientConnections)
				((void(*)(UNetConnection*, AActor*)) FindFlushDormancy())(Conn, Actor);
}

void UNetDriver::PostLoadHook()
{
	if (VersionInfo.EngineVersion == 4.16)
	{
		NetworkObjectListOffset = 0x3f8;
		ReplicationFrameOffset = 0x288;
	}
	else if (VersionInfo.EngineVersion == 4.19)
	{
		NetworkObjectListOffset = 0x490;
		ReplicationFrameOffset = 0x2c8;
	}
	else if (VersionInfo.FortniteVersion >= 2.5 && VersionInfo.FortniteVersion <= 3.1)
	{
		NetworkObjectListOffset = VersionInfo.FortniteVersion == 3.1 ? 0x4F8 : 0x4F0;
		ReplicationFrameOffset = 0x328;
	}
	else if (VersionInfo.FortniteVersion <= 3.3)
	{
		NetworkObjectListOffset = VersionInfo.FortniteVersion == 3.3 ? 0x508 : 0x500;
		ReplicationFrameOffset = 0x330;
	}
	else if (VersionInfo.FortniteVersion >= 20.40 && VersionInfo.FortniteVersion < 22)
	{
		NetworkObjectListOffset = 0x6b8;
		ReplicationFrameOffset = 0x3d8;
	}
	else if (std::floor(VersionInfo.FortniteVersion) == 20)
	{
		NetworkObjectListOffset = 0x6c8;
		ReplicationFrameOffset = 0x3e0;
	}
	else if (std::floor(VersionInfo.FortniteVersion) == 22)
	{
		NetworkObjectListOffset = 0x708;
		ReplicationFrameOffset = 0x428;
	}
	else if (VersionInfo.FortniteVersion >= 23 && VersionInfo.FortniteVersion < 24.30)
	{
		ReplicationFrameOffset = VersionInfo.FortniteVersion == 24.20 ? 0x438 : 0x440;
		NetworkObjectListOffset = VersionInfo.FortniteVersion < 24 ? 0x720 : 0x730;
	}
	else if (VersionInfo.FortniteVersion >= 24.30 && VersionInfo.FortniteVersion < 28)
	{
		NetworkObjectListOffset = VersionInfo.FortniteVersion < 25.11 ? 0x738 : 0x750;
		ReplicationFrameOffset = VersionInfo.FortniteVersion < 25.11 ? 0x440 : 0x458;
	}
	else if (VersionInfo.FortniteVersion >= 28)
	{
		NetworkObjectListOffset = 0x760;
		ReplicationFrameOffset = 0x468;
	}

	if (VersionInfo.FortniteVersion <= 1.72 && VersionInfo.FortniteVersion != 1.1 && VersionInfo.FortniteVersion != 1.11)
		ClientWorldPackageNameOffset = 0x336A8;
	else if (VersionInfo.FortniteVersion == 1.8 || VersionInfo.FortniteVersion == 1.81 || VersionInfo.FortniteVersion == 1.82 || VersionInfo.FortniteVersion == 1.9)
		ClientWorldPackageNameOffset = 0x33788;
	else if (VersionInfo.FortniteVersion == 1.10)
		ClientWorldPackageNameOffset = 0x337A8;
	else if (VersionInfo.FortniteVersion == 1.11)
		ClientWorldPackageNameOffset = 0x337B8;
	else if (VersionInfo.FortniteVersion >= 2.2 && VersionInfo.FortniteVersion <= 2.4)
		ClientWorldPackageNameOffset = 0xA17A8;
	else if (VersionInfo.FortniteVersion == 2.42 || VersionInfo.FortniteVersion == 2.5)
		ClientWorldPackageNameOffset = 0x17F8;
	else if (VersionInfo.FortniteVersion == 3.1)
		ClientWorldPackageNameOffset = 0x1818;
	else if (VersionInfo.FortniteVersion == 3.2)
		ClientWorldPackageNameOffset = 0x1820;
	else if (VersionInfo.FortniteVersion == 3.3)
		ClientWorldPackageNameOffset = 0x1828;
	else if (VersionInfo.FortniteVersion < 24 && VersionInfo.FortniteVersion > 23.20)
		ClientWorldPackageNameOffset = 0x17D0;
	else if (VersionInfo.FortniteVersion >= 23 && VersionInfo.FortniteVersion <= 23.20)
		ClientWorldPackageNameOffset = 0x1780;
	else if (std::floor(VersionInfo.FortniteVersion) == 22)
		ClientWorldPackageNameOffset = 0x1730;
	else if (VersionInfo.FortniteVersion >= 28)
		ClientWorldPackageNameOffset = 0x1828;
	else if (VersionInfo.FortniteVersion >= 25.30)
		ClientWorldPackageNameOffset = 0x1820;
	else if (VersionInfo.EngineVersion == 5.2)
		ClientWorldPackageNameOffset = 0x1818;
	else if (VersionInfo.FortniteVersion >= 24)
		ClientWorldPackageNameOffset = 0x1820;
	else if (VersionInfo.FortniteVersion >= 20.20)
		ClientWorldPackageNameOffset = 0x16b8;
	else if (VersionInfo.FortniteVersion >= 20)
		ClientWorldPackageNameOffset = 0x1698;

	if (VersionInfo.FortniteVersion >= 25.10)
	{
		DestroyedStartupOrDormantActorsOffset = VersionInfo.FortniteVersion >= 28 ? 0x328 : 0x318;
		DestroyedStartupOrDormantActorGUIDsOffset = VersionInfo.FortniteVersion >= 28 ? 0x14b8 : (VersionInfo.FortniteVersion < 25.30 ? 0x14a8 : 0x14b0);
		ClientVisibleLevelNamesOffset = DestroyedStartupOrDormantActorGUIDsOffset + (VersionInfo.FortniteVersion < 24 ? 0x190 : 0x1e0);
	}
	else if (VersionInfo.FortniteVersion >= 23)
	{
		DestroyedStartupOrDormantActorsOffset = std::floor(VersionInfo.FortniteVersion) == 24 && VersionInfo.FortniteVersion < 24.30 ? 0x2f8 : 0x300;
		DestroyedStartupOrDormantActorGUIDsOffset = VersionInfo.EngineVersion == 5.2 ? 0x14a8 : 0x14b0;
		ClientVisibleLevelNamesOffset = DestroyedStartupOrDormantActorGUIDsOffset + (VersionInfo.FortniteVersion < 24 ? 0x190 : 0x1e0);
	}
	else if (VersionInfo.FortniteVersion >= 20.40)
	{
		DestroyedStartupOrDormantActorsOffset = 0x2e8;
		DestroyedStartupOrDormantActorGUIDsOffset = VersionInfo.EngineVersion >= 5.1 ? 0x14b0 : 0x1488;
		ClientVisibleLevelNamesOffset = DestroyedStartupOrDormantActorGUIDsOffset + (VersionInfo.EngineVersion >= 5.1 ? 0xf0 : 0xa0);
	}
	else if (VersionInfo.FortniteVersion >= 20)
	{
		DestroyedStartupOrDormantActorsOffset = 0x2f0;
		DestroyedStartupOrDormantActorGUIDsOffset = VersionInfo.FortniteVersion >= 20.20 ? 0x1488 : 0x1468;
		ClientVisibleLevelNamesOffset = DestroyedStartupOrDormantActorGUIDsOffset + 0xa0;
	}

	if (!FindServerReplicateActors())
	{
		if (VersionInfo.EngineVersion >= 5.3 && FConfiguration::bEnableIris)
		{
			FindSendClientAdjustment();
			FindUpdateIrisReplicationViews();
			FindPreSendUpdate();
			Utils::Hook(FindTickFlush(), TickFlush__Iris, TickFlushOG);
			return;
		}
		// cache
		FindCreateChannel();
		FindSetChannelActor();
		FindReplicateActor();
		FindSendClientAdjustment();
		FindIsNetRelevantForVft();
		FindCallPreReplication();
		FindCloseActorChannel();
		FindStartBecomingDormant();
		FindClientHasInitializedLevelFor();
		FindSetChannelActorForDestroy();
		FindSendDestructionInfo();
		FindIsNetReady();

		if (VersionInfo.FortniteVersion < 3.4)
			FindFlushDormancy();
		else
		{
			FindGetNamePool();
		}

		GetActorLocation = (void(*)(AActor*, FFrame&, FVector*))AActor::GetDefaultObj()->GetFunction("K2_GetActorLocation")->GetNativeFunc();

		Utils::Hook(FindTickFlush(), TickFlush, TickFlushOG);
	}
	else
	{
		ServerReplicateActors_ = FindServerReplicateActors();

		Utils::Hook(FindTickFlush(), TickFlush__RepGraph, TickFlushOG);
	}

	if (VersionInfo.FortniteVersion < 3.4 && FindFlushDormancy())
	{
		Utils::Hook(__int64(AActor::GetDefaultObj()->GetFunction("FlushNetDormancy")->GetImpl()), FlushNetDormancy, FlushNetDormancyOG);
		Utils::Hook(__int64(AActor::GetDefaultObj()->GetFunction("SetNetDormancy")->GetImpl()), SetNetDormancy, SetNetDormancyOG);
	}
}
