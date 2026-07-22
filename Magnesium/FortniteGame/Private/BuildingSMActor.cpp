#include "pch.h"
#include "../Public/BuildingSMActor.h"
#include "../../Engine/Public/DataTableFunctionLibrary.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/FortGameMode.h"
#include "../Public/FortWeapon.h"
#include "../Public/FortKismetLibrary.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../../Erbium/Public/Configuration.h"


void ABuildingSMActor::OnDamageServer(ABuildingSMActor* Actor, float Damage, FGameplayTagContainer DamageTags, FVector Momentum, __int64 HitInfo, AActor* InstigatedBy, AActor* DamageCauser, __int64 EffectContext)
{
	auto GameState = ((AFortGameStateAthena*)UWorld::GetWorld()->GameState);
	auto GameMode = ((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode);
	auto Controller = (AFortPlayerControllerAthena*)InstigatedBy;

	/*auto bIsWeakspot = Damage == 100.f && Actor->IsA<ABuildingSMActor>() && DamageCauser->IsA<AFortWeapon>() && ((AFortWeapon*)DamageCauser)->WeaponData->IsA(UFortWeaponMeleeItemDefinition::StaticClass());

	if (bIsWeakspot)
	{
		FGameplayTagContainer TargetTags{};
		auto Interface = (IGameplayTagAssetInterface*)Actor->GetInterface(IGameplayTagAssetInterface::StaticClass());
		if (Interface)
		{
			auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
			GetOwnedGameplayTags(Interface, &TargetTags);
			//Interface->GetOwnedGameplayTags(&TargetTags);
		}
		// add Quest.MetaData.HitWeakSpot to source tags
		if (Controller)
			Controller->GetQuestManager(1)->SendStatEvent(Controller, EFortQuestObjectiveStatEvent::GetComplexCustom(), 1, Actor, TargetTags);
	}*/

	if (!InstigatedBy || !Actor->IsA<ABuildingSMActor>() || Actor->bPlayerPlaced || Actor->GetHealth() == 1 || (Actor->HasbAllowResourceDrop() && !Actor->bAllowResourceDrop))
		return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);
	if (!DamageCauser || !DamageCauser->IsA<AFortWeapon>() || !((AFortWeapon*)DamageCauser)->WeaponData->IsA(UFortWeaponMeleeItemDefinition::StaticClass()))
		return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);
	// Harvest resources go to a player controller's world inventory - other
	// instigators (native AI bot controllers, pawns on some versions) have
	// no world inventory at this offset and crashed below.
	if (!InstigatedBy->IsA<AFortPlayerControllerAthena>() || !Controller->WorldInventory)
		return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);

	auto Resource = UFortKismetLibrary::K2_GetResourceItemDefinition(Actor->ResourceType);
	if (!Resource)
		return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);
	auto MaxMat = Resource->GetMaxStackSize();

	
    static auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;
	static auto GameData = Playlist ? Playlist->ResourceRates.Get() : nullptr;
	if (!GameData)
		GameData = FindObject<UCurveTable>(GameMode->HasWarmupRequiredPlayerCount() ? L"/Game/Athena/Balance/DataTables/AthenaResourceRates.AthenaResourceRates" : L"/Game/Balance/DataTables/ResourceRates.ResourceRates");

	int ResCount = 0;
	if (Actor->HasBuildingResourceAmountOverride())
	{
		FCurveTableRowHandle& BuildingResourceAmountOverride = Actor->BuildingResourceAmountOverride;

		if (BuildingResourceAmountOverride.RowName.ComparisonIndex > 0)
		{
			float Out = 0.f;
			UDataTableFunctionLibrary::EvaluateCurveTableRow(GameData, BuildingResourceAmountOverride.RowName, 0.f, nullptr, &Out, FString());

			float RC = Out / (Actor->GetMaxHealth() / Damage);

			ResCount = (int)round(RC);
		}
	}
	else
	{
		auto ClassData = Actor->GetClassData();
		FCurveTableRowHandle& BuildingResourceAmountOverride = ClassData->BuildingResourceAmountOverride;

		if (BuildingResourceAmountOverride.RowName.ComparisonIndex > 0)
		{
			float Out = 0.f;
			UDataTableFunctionLibrary::EvaluateCurveTableRow(GameData, BuildingResourceAmountOverride.RowName, 0.f, nullptr, &Out, FString());

			float RC = Out / (Actor->GetMaxHealth() / Damage);

			ResCount = (int)round(RC);
		}
	}

	if (ResCount > 0)
	{
		auto ItemP = Controller->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
			{
				return entry->ItemEntry.ItemDefinition == Resource;
			});
		auto itemEntry = Controller->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
			{
				return entry.ItemDefinition == Resource;
			}, FFortItemEntry::Size());

		if (ItemP)
		{
			auto Item = *ItemP;

			/*for (int i = 0; i < Item->ItemEntry.StateValues.Num(); i++)
			{
				auto& StateValue = Item->ItemEntry.StateValues.Get(i, FFortItemEntryStateValue::Size());

				if (StateValue.StateType != 2)
					continue;

				StateValue.IntValue = 0;
			}*/


			itemEntry->Count += ResCount;
			if (itemEntry->Count > MaxMat)
			{
				AFortInventory::SpawnPickup(Controller->Pawn->K2_GetActorLocation(), Item->ItemEntry.ItemDefinition, itemEntry->Count - MaxMat, 0, EFortPickupSourceTypeFlag::GetTossed(), EFortPickupSpawnSource::GetUnset(), Controller->MyFortPawn);
				itemEntry->Count = MaxMat;
			}

			for (int i = 0; i < itemEntry->StateValues.Num(); i++)
			{
				auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

				if (StateValue.StateType != 2)
					continue;

				StateValue.IntValue = 0;
				break;
			}


			Item->ItemEntry.Count = itemEntry->Count;
			Controller->WorldInventory->UpdateEntry(*itemEntry);
			Item->ItemEntry.bIsDirty = true;
		}
		else
		{
			if (ResCount > MaxMat)
			{
				AFortInventory::SpawnPickup(Controller->Pawn->K2_GetActorLocation(), Resource, ResCount - MaxMat, 0, EFortPickupSourceTypeFlag::GetTossed(), EFortPickupSpawnSource::GetUnset(), Controller->MyFortPawn);
				ResCount = MaxMat;
			}

			Controller->WorldInventory->GiveItem(Resource, ResCount, 0, 0, false);
		}
	}


	if (ResCount > 0)
		Controller->ClientReportDamagedResourceBuilding(Actor, ResCount == 0 ? EFortResourceType(EFortResourceType__Enum::GetNone()) : Actor->ResourceType, ResCount, Actor->GetHealth() - Damage <= 0, Damage == 100.f);

	Actor->ForceNetUpdate();
	return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy, DamageCauser, EffectContext);
}

uint32 SpawnDecoVft = 0;
uint32 ShouldAllowServerSpawnDecoVft = 0;
static std::unordered_map<UClass*, UFortDecoItemDefinition*> SavedTrapDefinitions;

void ABuildingSMActor::RegisterTrapDefinition(UClass* TrapClass, UFortDecoItemDefinition* ItemDefinition)
{
	if (TrapClass && ItemDefinition)
		SavedTrapDefinitions[TrapClass] = ItemDefinition;
}

UFortDecoItemDefinition* ABuildingSMActor::GetTrapDefinition(UClass* TrapClass)
{
	auto Match = SavedTrapDefinitions.find(TrapClass);
	return Match == SavedTrapDefinitions.end() ? nullptr : Match->second;
}

static bool ResolveDecoToolPlayer(AFortDecoTool* DecoTool, AFortPlayerPawnAthena*& Pawn, AFortPlayerControllerAthena*& PlayerController, AFortPlayerStateAthena*& PlayerState)
{
	Pawn = nullptr;
	PlayerController = nullptr;
	PlayerState = nullptr;

	if (!DecoTool)
		return false;

	auto Owner = DecoTool->Owner;
	if (Owner)
	{
		Pawn = Owner->Cast<AFortPlayerPawnAthena>();
		PlayerController = Owner->Cast<AFortPlayerControllerAthena>();
	}

	if (!Pawn && DecoTool->Instigator)
		Pawn = DecoTool->Instigator->Cast<AFortPlayerPawnAthena>();

	if (Pawn)
	{
		if (!PlayerController && Pawn->Controller)
			PlayerController = Pawn->Controller->Cast<AFortPlayerControllerAthena>();

		if (Pawn->PlayerState)
			PlayerState = Pawn->PlayerState->Cast<AFortPlayerStateAthena>();
	}

	if (PlayerController && !PlayerState)
		PlayerState = PlayerController->PlayerState;

	return PlayerController && PlayerState;
}

static bool ApplyTrapTeam(ABuildingSMActor* Trap, AFortPlayerStateAthena* PlayerState)
{
	if (!Trap || !PlayerState || !PlayerState->HasTeamIndex())
		return false;

	auto TeamIndex = PlayerState->TeamIndex;

	if (Trap->HasTeam())
		Trap->Team = TeamIndex;

	if (Trap->HasTeamIndex())
		Trap->TeamIndex = TeamIndex;

	if (Trap->HasOwnerPersistentID() && PlayerState->HasWorldPlayerId())
		Trap->OwnerPersistentID = PlayerState->WorldPlayerId;

	if (Trap->HasbPlayerPlaced())
		Trap->bPlayerPlaced = true;

	Trap->SetTeam(TeamIndex);
	Trap->ForceNetUpdate();
	return true;
}

static int GetAttachedBuildingActorCount(ABuildingSMActor* AttachedActor)
{
	if (!AttachedActor || !AttachedActor->HasAttachedBuildingActors())
		return 0;

	return AttachedActor->AttachedBuildingActors.Num();
}

static bool ApplyTrapTeamToNewAttachments(ABuildingSMActor* AttachedActor, AFortPlayerStateAthena* PlayerState,
	int PreviousAttachedActorCount, UFortDecoItemDefinition* ItemDefinition = nullptr)
{
	if (!AttachedActor || !AttachedActor->HasAttachedBuildingActors())
		return false;

	auto& AttachedBuildingActors = AttachedActor->AttachedBuildingActors;
	bool bApplied = false;

	for (int i = PreviousAttachedActorCount; i < AttachedBuildingActors.Num(); i++)
	{
		auto AttachedBuildingActor = AttachedBuildingActors.Get(i);
		bApplied = ApplyTrapTeam(AttachedBuildingActor, PlayerState) || bApplied;
		if (AttachedBuildingActor && ItemDefinition)
			ABuildingSMActor::RegisterTrapDefinition(AttachedBuildingActor->Class, ItemDefinition);
	}

	return bApplied;
}

static UFortDecoItemDefinition* ResolvePlacedDecoDefinition(AFortDecoTool* Tool, uint8 AttachmentType)
{
	if (!Tool)
		return nullptr;
	UObject* Selected = Tool->ItemDefinition;
	if (auto ContextTool = Tool->Cast<AFortDecoTool_ContextTrap>())
	{
		auto ContextDefinition = ContextTool->ContextTrapItemDefinition;
		if (!ContextDefinition || !SDK::MemReadable(ContextDefinition, sizeof(void*)))
			return nullptr;
		switch (AttachmentType)
		{
		case 0: case 6: Selected = ContextDefinition->FloorTrap; break;
		case 2: case 7: Selected = ContextDefinition->CeilingTrap; break;
		case 1: Selected = ContextDefinition->WallTrap; break;
		case 8: Selected = ContextDefinition->StairTrap; break;
		default: return nullptr;
		}
	}
	if (!Selected || !SDK::MemReadable(Selected, 0x40) || !Selected->Class ||
		!SDK::MemReadable(Selected->Class, 0x40) || !Selected->IsA<UFortDecoItemDefinition>())
		return nullptr;
	return static_cast<UFortDecoItemDefinition*>(Selected);
}

ABuildingSMActor* ABuildingSMActor::SpawnSavedTrap(UClass* TrapClass, const FVector& SavedLocation, const FRotator& SavedRotation,
	ABuildingSMActor* AttachedActor, uint8 AttachmentType, AFortPlayerControllerAthena* PlayerController, const wchar_t* ItemDefinitionPath)
{
	if (!TrapClass || !AttachedActor || !PlayerController || !PlayerController->PlayerState || !SpawnDecoVft)
		return nullptr;

	UObject* SavedItemObject = ItemDefinitionPath && *ItemDefinitionPath
		? (UObject*)SDK::StaticFindObject(ItemDefinitionPath, UObject::StaticClass()) : nullptr;
	UFortDecoItemDefinition* ItemDefinition = SavedItemObject && SavedItemObject->IsA<UFortDecoItemDefinition>()
		? (UFortDecoItemDefinition*)SavedItemObject : nullptr;
	if (!ItemDefinition)
		ItemDefinition = GetTrapDefinition(TrapClass);
	if (!SavedItemObject)
		SavedItemObject = ItemDefinition;
	// Item definitions are UObjects, not actors; UGameplayStatics::GetAllActorsOfClass
	// will always return an empty list for them. Search the loaded object registry.
	for (int Index = 0; !ItemDefinition && Index < TUObjectArray::Num(); ++Index)
	{
		auto Object = TUObjectArray::GetObjectByIndex(Index);
		auto Definition = Object && Object->IsA<UFortDecoItemDefinition>() ? (UFortDecoItemDefinition*)Object : nullptr;
		if (Definition && Definition->BlueprintClass.Get() == TrapClass)
		{
			ItemDefinition = Definition;
			break;
		}
	}
	auto TrapToolClass = FindClass("FortTrapTool");
	if (!TrapToolClass)
		return nullptr;

	auto Tool = UWorld::SpawnActor<AFortDecoTool>(TrapToolClass, FVector{}, FRotator{}, PlayerController);
	if (!Tool)
		return nullptr;

	// SpawnDeco receives the concrete trap class and supporting build directly. The
	// item definition improves parity when available, but is not required by legacy
	// versions and must not prevent old saves from restoring their attachment.
	Tool->ItemDefinition = SavedItemObject;
	Tool->Owner = PlayerController->MyFortPawn ? (AActor*)PlayerController->MyFortPawn : (AActor*)PlayerController;
	Tool->Instigator = PlayerController->MyFortPawn;

	FVector Location = SavedLocation;
	FRotator Rotation = SavedRotation;
	ABuildingSMActor* Trap = nullptr;
	if (VersionInfo.FortniteVersion < 18)
	{
		// Legacy SpawnDeco vtable layouts are not stable enough for a direct native call.
		// Let ProcessEvent marshal the version's real signature, then obtain the child
		// that Fortnite appended to the supporting build.
		if (SavedItemObject && SavedItemObject->IsA<UFortItemDefinition>() && PlayerController->WorldInventory)
		{
			// The original legacy placement event verifies inventory and consumes one
			// trap. Supply that transient placement item so restore follows the same path.
			PlayerController->WorldInventory->GiveItem((UFortItemDefinition*)SavedItemObject, 1);
			const int PreviousCount = GetAttachedBuildingActorCount(AttachedActor);
			Tool->ServerSpawnDeco(Location, Rotation, AttachedActor, AttachmentType);
			if (AttachedActor->HasAttachedBuildingActors() && AttachedActor->AttachedBuildingActors.Num() > PreviousCount)
				Trap = AttachedActor->AttachedBuildingActors.Get(PreviousCount);
		}
	}
	else if (VersionInfo.FortniteVersion >= 27)
	{
		auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, TSubclassOf<ABuildingSMActor>&, FVector&, FRotator&, ABuildingSMActor*, uint8_t, int))Tool->Vft[SpawnDecoVft];
		TSubclassOf<ABuildingSMActor> TrapSubclass;
		TrapSubclass.ClassPtr = TrapClass;
		if (SpawnDeco)
			Trap = SpawnDeco(Tool, TrapSubclass, Location, Rotation, AttachedActor, AttachmentType, 0);
	}
	else
	{
		auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, UClass*, FVector&, FRotator&, ABuildingSMActor*, uint8_t, int))Tool->Vft[SpawnDecoVft];
		if (SpawnDeco)
			Trap = SpawnDeco(Tool, TrapClass, Location, Rotation, AttachedActor, AttachmentType, 0);
	}

	if (!Trap || !SDK::MemReadable(Trap, 0x40) || !Trap->Class || !SDK::MemReadable(Trap->Class, 0x40))
	{
		Tool->K2_DestroyActor();
		return nullptr;
	}
	ApplyTrapTeam(Trap, static_cast<AFortPlayerStateAthena*>(PlayerController->PlayerState));
	if (ItemDefinition)
		RegisterTrapDefinition(TrapClass, ItemDefinition);
	Tool->K2_DestroyActor();
	return Trap;
}

void AFortDecoTool::ServerSpawnDeco_(UObject* Context, FFrame& Stack)
{
	FVector Location;
	FRotator Rotation;
	ABuildingSMActor* AttachedActor;
	uint8_t InBuildingAttachmentType;
	Stack.StepCompiledIn(&Location);
	Stack.StepCompiledIn(&Rotation);
	Stack.StepCompiledIn(&AttachedActor);
	Stack.StepCompiledIn(&InBuildingAttachmentType);
	Stack.IncrementCode();
	auto DecoTool = (AFortDecoTool*)Context;

	AFortPlayerPawnAthena* Pawn = nullptr;
	AFortPlayerControllerAthena* PlayerController = nullptr;
	AFortPlayerStateAthena* PlayerState = nullptr;
	if (!ResolveDecoToolPlayer(DecoTool, Pawn, PlayerController, PlayerState))
		return;

	if (VersionInfo.FortniteVersion >= 18) // idk when they stripped it, guessing s18
	{
		auto ItemDefinition = (UFortDecoItemDefinition*)DecoTool->ItemDefinition;

		if (auto ContextTrapTool = DecoTool->Cast<AFortDecoTool_ContextTrap>()) {
			switch ((int)InBuildingAttachmentType) {
			case 0:
			case 6:
				ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->FloorTrap;
				break;
			case 7:
			case 2:
				ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->CeilingTrap;
				break;
			case 1:
				ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->WallTrap;
				break;
			case 8:
				ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->StairTrap;
				break;
			}
		}

		if (!ItemDefinition)
			return;

		auto ShouldAllowServerSpawnDeco = (bool (*)(AFortDecoTool*, FVector&, FRotator&, ABuildingSMActor*, uint8_t)) DecoTool->Vft[ShouldAllowServerSpawnDecoVft];

		if (ShouldAllowServerSpawnDecoVft && !ShouldAllowServerSpawnDeco(DecoTool, Location, Rotation, AttachedActor, InBuildingAttachmentType))
			return;

		auto PreviousAttachedActorCount = GetAttachedBuildingActorCount(AttachedActor);
		ABuildingSMActor* NewTrap = nullptr;
		if (VersionInfo.FortniteVersion >= 27)
		{
			auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, TSubclassOf<ABuildingSMActor>&, FVector&, FRotator&, ABuildingSMActor*, uint8_t, int)) DecoTool->Vft[SpawnDecoVft];

			TSubclassOf<ABuildingSMActor> SubclassOf;
			SubclassOf.ClassPtr = ItemDefinition->BlueprintClass.Get();
			NewTrap = SpawnDecoVft && SpawnDeco ? SpawnDeco(DecoTool, SubclassOf, Location, Rotation, AttachedActor, InBuildingAttachmentType, 0) : nullptr;
		}
		else
		{
			auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, UClass*, FVector&, FRotator&, ABuildingSMActor*, uint8_t, int)) DecoTool->Vft[SpawnDecoVft];

			NewTrap = SpawnDecoVft && SpawnDeco ? SpawnDeco(DecoTool, ItemDefinition->BlueprintClass.Get(), Location, Rotation, AttachedActor, InBuildingAttachmentType, 0) : nullptr;
		}

		ApplyTrapTeam(NewTrap, PlayerState);
		ABuildingSMActor::RegisterTrapDefinition(NewTrap ? NewTrap->Class : ItemDefinition->BlueprintClass.Get(), ItemDefinition);
		ApplyTrapTeamToNewAttachments(AttachedActor, PlayerState, PreviousAttachedActorCount);

		auto Resource = UFortKismetLibrary::GetDefaultObj()->K2_GetResourceItemDefinition(AttachedActor->ResourceType);
		auto item = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Item) {
			return Item->ItemEntry.ItemDefinition == DecoTool->ItemDefinition;
			});
		auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry) {
			return entry.ItemDefinition == DecoTool->ItemDefinition;
			}, FFortItemEntry::Size());
		if (!itemEntry)
			return;

		itemEntry->Count--;
		if (itemEntry->Count <= 0)
			PlayerController->WorldInventory->Remove(itemEntry->ItemGuid);
		else
		{
			(*item)->ItemEntry.Count = itemEntry->Count;
			PlayerController->WorldInventory->UpdateEntry(*itemEntry);
			(*item)->ItemEntry.bIsDirty = true;
		}
	}

	if (VersionInfo.FortniteVersion < 18)
	{
		auto PreviousAttachedActorCount = GetAttachedBuildingActorCount(AttachedActor);
		callOG(DecoTool, Stack.GetCurrentNativeFunction(), ServerSpawnDeco, Location, Rotation, AttachedActor, InBuildingAttachmentType);
		ApplyTrapTeamToNewAttachments(AttachedActor, PlayerState, PreviousAttachedActorCount,
			ResolvePlacedDecoDefinition(DecoTool, InBuildingAttachmentType));
	}
}

void AFortDecoTool_ContextTrap::ServerSpawnDeco_Implementation(UObject* Context, FFrame& Stack)
{
	auto& Location = *(FVector*)Stack.Locals;
	auto& Rotation = *(FRotator*)(__int64(Stack.Locals) + FVector::Size());
	auto& AttachedActor = *(ABuildingSMActor**)(__int64(Stack.Locals) + FVector::Size() + FRotator::Size());
	auto& InBuildingAttachmentType = *(uint8_t*)(__int64(Stack.Locals) + FVector::Size() + FRotator::Size() + 8);
	auto DecoTool = (AFortDecoTool_ContextTrap*)Context;

	AFortPlayerPawnAthena* Pawn = nullptr;
	AFortPlayerControllerAthena* PlayerController = nullptr;
	AFortPlayerStateAthena* PlayerState = nullptr;
	if (!ResolveDecoToolPlayer(DecoTool, Pawn, PlayerController, PlayerState))
		return;

	if (VersionInfo.FortniteVersion >= 18) // idk when they stripped it, guessing s18
	{
		auto ItemDefinition = (UFortDecoItemDefinition*)DecoTool->ItemDefinition;

		if (auto ContextTrapTool = DecoTool->Cast<AFortDecoTool_ContextTrap>()) {
			switch ((int)InBuildingAttachmentType) {
			case 0:
			case 6:
				ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->FloorTrap;
				break;
			case 7:
			case 2:
				ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->CeilingTrap;
				break;
			case 1:
				ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->WallTrap;
				break;
			case 8:
				ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->StairTrap;
				break;
			}
		}

		if (!ItemDefinition)
			return;

		auto ShouldAllowServerSpawnDeco = (bool (*)(AFortDecoTool*, FVector&, FRotator&, ABuildingSMActor*, uint8_t)) DecoTool->Vft[ShouldAllowServerSpawnDecoVft];

		if (ShouldAllowServerSpawnDecoVft && !ShouldAllowServerSpawnDeco(DecoTool, Location, Rotation, AttachedActor, InBuildingAttachmentType))
			return;

		auto PreviousAttachedActorCount = GetAttachedBuildingActorCount(AttachedActor);
		ABuildingSMActor* NewTrap = nullptr;
		if (VersionInfo.FortniteVersion >= 27)
		{
			auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, TSubclassOf<ABuildingSMActor>&, FVector&, FRotator&, ABuildingSMActor*, uint8_t, int)) DecoTool->Vft[SpawnDecoVft];

			TSubclassOf<ABuildingSMActor> SubclassOf;
			SubclassOf.ClassPtr = ItemDefinition->BlueprintClass.Get();
			NewTrap = SpawnDecoVft && SpawnDeco ? SpawnDeco(DecoTool, SubclassOf, Location, Rotation, AttachedActor, InBuildingAttachmentType, 0) : nullptr;
		}
		else
		{
			auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, UClass*, FVector&, FRotator&, ABuildingSMActor*, uint8_t, int)) DecoTool->Vft[SpawnDecoVft];

			NewTrap = SpawnDecoVft && SpawnDeco ? SpawnDeco(DecoTool, ItemDefinition->BlueprintClass.Get(), Location, Rotation, AttachedActor, InBuildingAttachmentType, 0) : nullptr;
		}

		ApplyTrapTeam(NewTrap, PlayerState);
		ABuildingSMActor::RegisterTrapDefinition(NewTrap ? NewTrap->Class : ItemDefinition->BlueprintClass.Get(), ItemDefinition);
		ApplyTrapTeamToNewAttachments(AttachedActor, PlayerState, PreviousAttachedActorCount);

		auto Resource = UFortKismetLibrary::GetDefaultObj()->K2_GetResourceItemDefinition(AttachedActor->ResourceType);
		auto item = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Item) {
			return Item->ItemEntry.ItemDefinition == DecoTool->ItemDefinition;
			});
		auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry) {
			return entry.ItemDefinition == DecoTool->ItemDefinition;
			}, FFortItemEntry::Size());
		if (!itemEntry)
			return;

		itemEntry->Count--;
		if (itemEntry->Count <= 0)
			PlayerController->WorldInventory->Remove(itemEntry->ItemGuid);
		else
		{
			(*item)->ItemEntry.Count = itemEntry->Count;
			PlayerController->WorldInventory->UpdateEntry(*itemEntry);
			(*item)->ItemEntry.bIsDirty = true;
		}
	}

	if (VersionInfo.FortniteVersion < 18)
	{
		auto PreviousAttachedActorCount = GetAttachedBuildingActorCount(AttachedActor);
		ServerSpawnDeco_ImplementationOG(Context, Stack);
		ApplyTrapTeamToNewAttachments(AttachedActor, PlayerState, PreviousAttachedActorCount,
			ResolvePlacedDecoDefinition(DecoTool, InBuildingAttachmentType));
	}
}


uint8 GetBuildingTypeFromBuildingAttachmentType(uint8 BuildingAttachmentType)
{
	if (uint8(BuildingAttachmentType) <= 7)
	{
		LONG Val = 0xC5;
		if (BitTest(&Val, uint8(BuildingAttachmentType)))
			return 1;
	}
	if (BuildingAttachmentType == 1)
		return 0;
	return 12;
}

extern uint64_t PayBuildableClassPlacementCost_;
extern uint64_t CanAffordToPlaceBuildableClass_;
extern uint64_t CantBuild_;
void AFortDecoTool::ServerCreateBuildingAndSpawnDeco(UObject* Context, FFrame& Stack)
{
	FVector BuildingLocation{};
	FRotator BuildingRotation{};
	FVector Location{};
	FRotator Rotation{};
	uint8_t InBuildingAttachmentType = 0;
	bool bSpawnDecoOnExtraPiece = false;
	FVector BuildingExtraPieceLocation{};
	auto NativeFunction = Stack.GetCurrentNativeFunction();
	Stack.StepCompiledIn(&BuildingLocation);
	Stack.StepCompiledIn(&BuildingRotation);
	Stack.StepCompiledIn(&Location);
	Stack.StepCompiledIn(&Rotation);
	Stack.StepCompiledIn(&InBuildingAttachmentType);
	// Extra-piece parameters were added after the original five-parameter RPC.
	// Stepping fields which are absent advances legacy stacks into unrelated data.
	if (NativeFunction && NativeFunction->GetProperty("bSpawnDecoOnExtraPiece"))
		Stack.StepCompiledIn(&bSpawnDecoOnExtraPiece);
	if (NativeFunction && NativeFunction->GetProperty("BuildingExtraPieceLocation"))
		Stack.StepCompiledIn(&BuildingExtraPieceLocation);
	Stack.IncrementCode();

	auto Tool = (AFortDecoTool*)Context;
	if (!Tool)
		return;

	auto Pawn = (AFortPlayerPawnAthena*)Tool->Owner;
	if (!Pawn)
		return;

	auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;
	if (!PlayerController)
		return;

	auto PlayerState = (AFortPlayerStateAthena*)Pawn->PlayerState;
	if (!PlayerState)
		return;

	auto ItemDefinition = (UFortDecoItemDefinition*)Tool->ItemDefinition;


	if (auto ContextTrapTool = Tool->Cast<AFortDecoTool_ContextTrap>()) {
		if (!ContextTrapTool->ContextTrapItemDefinition || !SDK::MemReadable(ContextTrapTool->ContextTrapItemDefinition, sizeof(void*)))
			return;
		switch ((int)InBuildingAttachmentType) {
		case 0:
		case 6:
			ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->FloorTrap;
			break;
		case 7:
		case 2:
			ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->CeilingTrap;
			break;
		case 1:
			ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->WallTrap;
			break;
		case 8:
			ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->StairTrap;
			break;
		}
	}
	if (!ItemDefinition || !SDK::MemReadable(ItemDefinition, 0x40) ||
		!ItemDefinition->Class || !SDK::MemReadable(ItemDefinition->Class, 0x40) ||
		!ItemDefinition->IsA<UFortDecoItemDefinition>())
		return;

	TArray<const UObject*> AutoCreateAttachmentBuildingShapes;
	for (auto& AutoCreateAttachmentBuildingShape : ItemDefinition->AutoCreateAttachmentBuildingShapes)
	{
		AutoCreateAttachmentBuildingShapes.Add(AutoCreateAttachmentBuildingShape);
	}

	auto World = UWorld::GetWorld();
	auto GameState = World ? (AFortGameStateAthena*)World->GameState : nullptr;
	if (!GameState)
	{
		AutoCreateAttachmentBuildingShapes.Free();
		return;
	}
	auto bIgnoreCanAffordCheck = UFortKismetLibrary::DoesItemDefinitionHaveGameplayTag(ItemDefinition, FGameplayTag(FName(L"Trap.ExtraPiece.Cost.Ignore")));
	TSubclassOf<AActor> BuildingClass{};
	EFortResourceType ResourceType = PlayerController->CurrentResourceType;

	if (ItemDefinition->HasAutoCreateAttachmentBuildingResourceType() && ItemDefinition->AutoCreateAttachmentBuildingResourceType != EFortResourceType(EFortResourceType__Enum::GetNone()))
		ResourceType = ItemDefinition->AutoCreateAttachmentBuildingResourceType;
	auto BuildingType = GetBuildingTypeFromBuildingAttachmentType(InBuildingAttachmentType);

	for (auto Shape : AutoCreateAttachmentBuildingShapes)
	{
		for (auto& Class : GameState->AllPlayerBuildableClasses)
		{
			if (!Class || !Class->GetDefaultObj())
				continue;
			auto Default = (ABuildingSMActor*)Class->GetDefaultObj();

			UObject* EditModePatternData = nullptr;

			if (Default->HasEditModePatternData())
				EditModePatternData = Default->EditModePatternData;
			else
			{
				auto ClassData = Default->GetClassData();
				if (ClassData)
					EditModePatternData = ClassData->EditModePatternData;
			}

			if (Default->ResourceType == ResourceType && Default->BuildingType == BuildingType && EditModePatternData == Shape)
			{
				BuildingClass = Class;
				goto _out;
			}
		}
	}

	return;
_out:

	FBuildingClassData BuildingClassData;
	BuildingClassData.BuildingClass = BuildingClass;
	BuildingClassData.PreviousBuildingLevel = -1;

	static auto UpgradeLevelOffset = FBuildingClassData::StaticStruct()->GetOffset("UpgradeLevel");
	if (VersionInfo.EngineVersion >= 5.3)
		*(uint8*)(__int64(&BuildingClassData) + UpgradeLevelOffset) = 0;
	else
		*(uint32*)(__int64(&BuildingClassData) + UpgradeLevelOffset) = 0;

	UFortWorldItem* Item = nullptr;
	if (!FConfiguration::bInfiniteMats)
	{
		auto CanAffordToPlaceBuildableClass = (bool(*)(AFortPlayerControllerAthena*, FBuildingClassData)) CanAffordToPlaceBuildableClass_;

		if (CanAffordToPlaceBuildableClass)
		{
			if (!CanAffordToPlaceBuildableClass(PlayerController, BuildingClassData))
				return;
		}
		else if (!PlayerController->bBuildFree && !FConfiguration::bInfiniteMats)
		{
			auto Resource = UFortKismetLibrary::K2_GetResourceItemDefinition(((ABuildingSMActor*)BuildingClass->GetDefaultObj())->ResourceType);

			auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
				{ return entry->ItemEntry.ItemDefinition == Resource; });

			if (!ItemP)
				return;

			Item = *ItemP;

			if (Item->ItemEntry.Count < 10)
				return;
		}
	}

	struct _Pad_0xC
	{
		uint8_t Padding[0xC];
	};
	struct _Pad_0x18
	{
		uint8_t Padding[0x18];
	};

	TArray<ABuildingSMActor*> RemoveBuildings;
	if (VersionInfo.FortniteVersion >= 27)
	{
		char _Unk_OutVar1;
		auto CantBuild = (__int64 (*)(UWorld*, TSubclassOf<AActor>&, _Pad_0x18, _Pad_0x18, bool, TArray<ABuildingSMActor*> *, char*))CantBuild_;

		if (CantBuild(UWorld::GetWorld(), BuildingClass, *(_Pad_0x18*)&BuildingLocation, *(_Pad_0x18*)&BuildingRotation, false, &RemoveBuildings, &_Unk_OutVar1))
			return;
	}
	else
	{
		char _Unk_OutVar1;
		auto CantBuild = (__int64 (*)(UWorld*, const UClass*, _Pad_0xC, _Pad_0xC, bool, TArray<ABuildingSMActor*> *, char*))CantBuild_;
		auto CantBuildNew = (__int64 (*)(UWorld*, const UClass*, _Pad_0x18, _Pad_0x18, bool, TArray<ABuildingSMActor*> *, char*))CantBuild_;

		if (VersionInfo.FortniteVersion >= 20.00 ? CantBuildNew(UWorld::GetWorld(), BuildingClass, *(_Pad_0x18*)&BuildingLocation, *(_Pad_0x18*)&BuildingRotation, false, &RemoveBuildings, &_Unk_OutVar1) : CantBuild(UWorld::GetWorld(), BuildingClass, *(_Pad_0xC*)&BuildingLocation, *(_Pad_0xC*)&BuildingRotation, false, &RemoveBuildings, &_Unk_OutVar1))
			return;
	}

	for (auto& RemoveBuilding : RemoveBuildings)
		RemoveBuilding->K2_DestroyActor();
	RemoveBuildings.Free();

	static auto K2_SpawnBuildingActor = ABuildingSMActor::GetDefaultObj()->GetFunction("K2_SpawnBuildingActor");

	ABuildingSMActor* Building = nullptr;
	/*if (K2_SpawnBuildingActor)
	{
		FTransform SpawnTransform(BuildLoc, BuildRot);
		Building = ABuildingSMActor::K2_SpawnBuildingActor(PlayerController, BuildingClass, SpawnTransform, PlayerController, nullptr, false, false);
	}
	else*/
	Building = UWorld::SpawnActorUnfinished<ABuildingSMActor>(BuildingClass, BuildingLocation, BuildingRotation, PlayerController);
	if (!Building)
		return;
	Building->InitializeKismetSpawnedBuildingActor(Building, PlayerController, true, nullptr, false);
	UWorld::FinishSpawnActor(Building, BuildingLocation, BuildingRotation);

	Building->bPlayerPlaced = true;

	if (((AFortPlayerStateAthena*)PlayerController->PlayerState)->HasTeamIndex())
		Building->Team = ((AFortPlayerStateAthena*)PlayerController->PlayerState)->TeamIndex;

	if (Building->HasTeamIndex())
		Building->TeamIndex = Building->Team;

	if (Building->HasOwnerPersistentID() && PlayerState->HasWorldPlayerId())
		Building->OwnerPersistentID = PlayerState->WorldPlayerId;

	//UWorld::FinishSpawnActor(Building, BuildLoc, BuildRot);

	if (!PlayerController->bBuildFree && !FConfiguration::bInfiniteMats)
	{
		auto PayBuildableClassPlacementCost = (int(*)(AFortPlayerControllerAthena*, FBuildingClassData)) PayBuildableClassPlacementCost_;
		if (PayBuildableClassPlacementCost)
			PayBuildableClassPlacementCost(PlayerController, BuildingClassData);
	}

	/*Building->Team = ((AFortPlayerStateAthena*)PlayerController->PlayerState)->TeamIndex;
	if (Building->HasTeamIndex())
		Building->TeamIndex = Building->Team;*/

	Tool->ServerSpawnDeco(Location, Rotation, Building, InBuildingAttachmentType);
}

void ABuildingSMActor::PostLoadHook()
{
	SDK::DbgLog("  [BSM] 0 start (CDO=%p)\n", (void*)GetDefaultObj());
	bool _hasOverride = GetDefaultObj() && GetDefaultObj()->HasBuildingResourceAmountOverride();
	SDK::DbgLog("  [BSM] 0a HasBuildingResourceAmountOverride=%d\n", (int)_hasOverride);
	if (!_hasOverride)
	{
		SDK::DbgLog("  [BSM] 0b pre-FindPattern#1\n");
		GetSparseClassData_ = Memcury::Scanner::FindPattern("48 83 EC ? 48 8B 81 ? ? ? ? 45 33 C0 4C 8B C9").Get();
		SDK::DbgLog("  [BSM] 0c FP#1=%p\n", (void*)GetSparseClassData_);

		if (!GetSparseClassData_)
			GetSparseClassData_ = Memcury::Scanner::FindPattern("48 83 EC ? 48 8B 81 ? ? ? ? 48 85 C0 74 ? 48 83 C4 ? C3").Get();
		SDK::DbgLog("  [BSM] 0d FP#2 done GSCD=%p\n", (void*)GetSparseClassData_);

		if (!GetSparseClassData_)
			GetSparseClassData_ = Memcury::Scanner::FindPattern("48 83 EC ? 48 8B 81 ? ? ? ? 45 33 C0 48 85 C0 75").Get();
		SDK::DbgLog("  [BSM] 0e FP#3 done GSCD=%p\n", (void*)GetSparseClassData_);
	}
	SDK::DbgLog("  [BSM] 1 sparse-class-data done\n");
	if (VersionInfo.FortniteVersion >= 18)
	{
		SpawnDecoVft = FindSpawnDecoVft();
		SDK::DbgLog("  [BSM] 2 SpawnDecoVft=%p\n", (void*)SpawnDecoVft);
		ShouldAllowServerSpawnDecoVft = FindShouldAllowServerSpawnDecoVft();
		SDK::DbgLog("  [BSM] 3 ShouldAllowServerSpawnDecoVft=%p\n", (void*)ShouldAllowServerSpawnDecoVft);
	}
	else
	{
		// The native SpawnDeco vtable path does not exist on the early
		// ServerSpawnDeco implementation. Match Erbium's version gate instead
		// of running the unsupported finder during UE 4.16 startup.
		SpawnDecoVft = 0;
		ShouldAllowServerSpawnDecoVft = 0;
		SDK::DbgLog("  [BSM] 2 legacy deco path; native vtable lookup skipped\n");
	}

	auto OnDamageServerAddr = FindFunctionCall(L"OnDamageServer", VersionInfo.EngineVersion == 4.16 ? std::vector<uint8_t>{ 0x4C, 0x89, 0x4C } : VersionInfo.EngineVersion == 4.19 || VersionInfo.EngineVersion >= 4.27 ? std::vector<uint8_t>{ 0x48, 0x8B, 0xC4 } : std::vector<uint8_t>{ 0x40, 0x55 });
	SDK::DbgLog("  [BSM] 4 OnDamageServerAddr=%p\n", (void*)OnDamageServerAddr);

	Utils::Hook(OnDamageServerAddr, OnDamageServer, OnDamageServerOG);
	SDK::DbgLog("  [BSM] 5 OnDamageServer hooked\n");

	auto ServerSpawnDecoFunc = AFortDecoTool::GetDefaultObj()->GetFunction("ServerSpawnDeco");
	if (ServerSpawnDecoFunc)
		Utils::ExecHook(ServerSpawnDecoFunc, AFortDecoTool::ServerSpawnDeco_, AFortDecoTool::ServerSpawnDeco_OG);

	auto CreateBuildingAndSpawnDecoFunc =
		AFortDecoTool::GetDefaultObj()->GetFunction("ServerCreateBuildingAndSpawnDeco");
	auto GameStateClass = AFortGameStateAthena::StaticClass();
	const bool bSupportsCustomCreateBuildingAndSpawnDeco =
		CreateBuildingAndSpawnDecoFunc &&
		CreateBuildingAndSpawnDecoFunc->GetProperty("BuildingLocation") &&
		GameStateClass && GameStateClass->GetProperty("AllPlayerBuildableClasses");
	if (bSupportsCustomCreateBuildingAndSpawnDeco)
	{
		Utils::ExecHook(CreateBuildingAndSpawnDecoFunc,
			AFortDecoTool::ServerCreateBuildingAndSpawnDeco,
			AFortDecoTool::ServerCreateBuildingAndSpawnDecoOG);
	}
	else
	{
		// 7.40's RPC contains only Location, Rotation and attachment type, and its
		// game state has no AllPlayerBuildableClasses array. Its native routine is
		// the compatible path and ultimately reaches our legacy ServerSpawnDeco
		// wrapper for team/definition bookkeeping.
		SDK::DbgLog("  [BSM] legacy ServerCreateBuildingAndSpawnDeco left native\n");
	}
	if (AFortDecoTool_ContextTrap::StaticClass())
	{
		auto Func = AFortDecoTool_ContextTrap::GetDefaultObj()->GetFunction("ServerSpawnDeco");

		if (!Func)
			Func = AFortDecoTool_ContextTrap::GetDefaultObj()->GetFunction("ServerSpawnDeco_Implementation");

		if (Func && Func != ServerSpawnDecoFunc)
			Utils::ExecHook(Func, AFortDecoTool_ContextTrap::ServerSpawnDeco_Implementation, AFortDecoTool_ContextTrap::ServerSpawnDeco_ImplementationOG);

		auto Func2 = AFortDecoTool_ContextTrap::GetDefaultObj()->GetFunction("ServerCreateBuildingAndSpawnDeco_Implementation");

		if (!Func2)
			Func2 = AFortDecoTool_ContextTrap::GetDefaultObj()->GetFunction("ServerCreateBuildingAndSpawnDeco");

		if (bSupportsCustomCreateBuildingAndSpawnDeco && Func2 &&
			Func2 != CreateBuildingAndSpawnDecoFunc)
		{
			Utils::ExecHook(Func2, AFortDecoTool::ServerCreateBuildingAndSpawnDeco,
				AFortDecoTool::ServerCreateBuildingAndSpawnDecoOG);
		}
	}
	SDK::DbgLog("  [BSM] 6 PostLoadHook complete\n");
}
