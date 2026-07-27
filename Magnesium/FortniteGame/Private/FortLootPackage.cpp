#include "pch.h"
#include "../Public/FortLootPackage.h"
#include "../Public/BuildingContainer.h"
#include "../Public/FortGameMode.h"
#include "../../Erbium/Public/Configuration.h"

struct FFortLootLevelData
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FFortLootLevelData);

	DEFINE_STRUCT_PROP(Category, FName);
	DEFINE_STRUCT_PROP(category, FName);
	DEFINE_STRUCT_PROP(LootLevel, int32);
	DEFINE_STRUCT_PROP(MinItemLevel, int32);
	DEFINE_STRUCT_PROP(MaxItemLevel, int32);
};

int GetLevel(const FDataTableCategoryHandle& CategoryHandle)
{
	auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
	auto GameState = (AFortGameStateAthena*)GameMode->GameState;

	if (!CategoryHandle.DataTable)
		return 0;

	if (!CategoryHandle.ColumnName)
		return 0;

	if (!CategoryHandle.RowContents.ComparisonIndex)
		return 0;

	int Level = 0;
	FFortLootLevelData* LootLevelData = nullptr;
	
	for (auto& LootLevelDataPair : (TMap<FName, FFortLootLevelData*>)CategoryHandle.DataTable->RowMap)
	{
		if (!LootLevelDataPair.Value())
			continue;

		if ((FFortLootLevelData::HasCategory() ? LootLevelDataPair.Value()->Category : LootLevelDataPair.Value()->category) != CategoryHandle.RowContents || LootLevelDataPair.Value()->LootLevel > GameState->WorldLevel || LootLevelDataPair.Value()->LootLevel <= Level)
			continue;

		Level = LootLevelDataPair.Value()->LootLevel;
		LootLevelData = LootLevelDataPair.Value();
	}

	if (LootLevelData)
	{
		auto subbed = LootLevelData->MaxItemLevel - LootLevelData->MinItemLevel;

		if (subbed <= -1)
			subbed = 0;
		else
		{
			auto calc = (int)(((float)rand() / 32767) * (float)(subbed + 1));
			if (calc <= subbed)
				subbed = calc;
		}

		return subbed + LootLevelData->MinItemLevel;
	}

	return 0;
}

struct FWeaponPickupAmmoCountData
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FWeaponPickupAmmoCountData);

	DEFINE_STRUCT_PROP(AmmoItemDefinitionTag, FGameplayTag);
	DEFINE_STRUCT_PROP(SpawnCount, FScalableFloat);
};

struct FHelperGameplayTagToAmmoCountMultiplier
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FHelperGameplayTagToAmmoCountMultiplier);

	DEFINE_STRUCT_PROP(Tag, FGameplayTag);
	DEFINE_STRUCT_PROP(CountMultiplier, FScalableFloat);
};

class UFortWeaponPickupSpawnAmmoData : public UObject
{
public:
	UCLASS_COMMON_MEMBERS(UFortWeaponPickupSpawnAmmoData);

	DEFINE_PROP(WeaponPickupAmmoCountArray, TArray<FWeaponPickupAmmoCountData>);
	DEFINE_PROP(DefaultWeaponAmmoMultiplier, FScalableFloat);
	DEFINE_PROP(WeaponPickupAmmoMultiplierOverrideArray, TArray<FHelperGameplayTagToAmmoCountMultiplier>);
	DEFINE_PROP(SourceToAmmoMultiplierOverrideArray, TArray<FHelperGameplayTagToAmmoCountMultiplier>);
};

void UFortLootPackage::SetupLDSForPackage(TArray<FFortItemEntry*>& LootDrops, SDK::FName Package, int i, FName TierGroup, int WorldLevel, ABuildingContainer* Container)
{
	TArray<FFortLootPackageData*> LPGroups{};

	for (auto const& Val : LootPackageMap[Package.ComparisonIndex])
	{
		if (!Val)
			continue;

		if (i != -1 && Val->LootPackageCategory != i)
			continue;

		if (WorldLevel >= 0)
		{
			if (Val->MaxWorldLevel >= 0 && WorldLevel > Val->MaxWorldLevel)
				continue;
			
			if (Val->MinWorldLevel >= 0 && WorldLevel < Val->MinWorldLevel)
				continue;
		}

		LPGroups.Add(Val);
	}

	if (LPGroups.Num() == 0)
		return;

	auto LootPackage = PickWeighted(LPGroups, [](float Total)
		{ return ((float)rand() / 32767.f) * Total; });
	if (!LootPackage)
		return;

	if (LootPackage->LootPackageCall.Num() > 1)
	{
		for (int i = 0; i < LootPackage->Count; i++)
			SetupLDSForPackage(LootDrops, UKismetStringLibrary::Conv_StringToName(LootPackage->LootPackageCall), 0, TierGroup, WorldLevel);

		return;
	}


	auto ItemDefinition = LootPackage->ItemDefinition.Get();

	if (!ItemDefinition)
		return;

	FFortItemEntry* AmmoEntry = nullptr;

	if (VersionInfo.FortniteVersion >= 11 && VersionInfo.FortniteVersion != 19.40)
	{
		if (auto WorldItemDefinition = ItemDefinition->Cast<UFortWorldItemDefinition>())
		{
			auto AmmoDef = WorldItemDefinition->GetAmmoWorldItemDefinition_BP();

			if (auto AmmoDefinition = AmmoDef->Cast<UFortAmmoItemDefinition>())
			{
				auto SpawnAmmoData = FindObject<UFortWeaponPickupSpawnAmmoData>(L"/Game/Athena/Balance/Pickups/FortWeaponPickupSpawnAmmoData.FortWeaponPickupSpawnAmmoData");

				FGameplayTagContainer AmmoTags{};

				auto Interface = (IGameplayTagAssetInterface*)AmmoDefinition->GetInterface(IGameplayTagAssetInterface::StaticClass());
				if (Interface)
				{
					auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
					GetOwnedGameplayTags(Interface, &AmmoTags);
					//Interface->GetOwnedGameplayTags(&TargetTags);
				}

				float AmmoCount = 0.f;
				for (int i = 0; i < SpawnAmmoData->WeaponPickupAmmoCountArray.Num(); i++)
				{
					auto& AmmoCountData = SpawnAmmoData->WeaponPickupAmmoCountArray.Get(i, FWeaponPickupAmmoCountData::Size());

					if (AmmoTags.HasTag(AmmoCountData.AmmoItemDefinitionTag))
					{
						AmmoCount = AmmoCountData.SpawnCount.Evaluate((float)WorldLevel);
						break;
					}
				}

				auto Multiplier = SpawnAmmoData->DefaultWeaponAmmoMultiplier.Evaluate((float)WorldLevel);

				FGameplayTagContainer WeaponTags{};

				auto Interface2 = (IGameplayTagAssetInterface*)ItemDefinition->GetInterface(IGameplayTagAssetInterface::StaticClass());
				if (Interface2)
				{
					auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface2->Vft[0x2];
					GetOwnedGameplayTags(Interface2, &WeaponTags);
					//Interface->GetOwnedGameplayTags(&TargetTags);
				}

				for (int i = 0; i < SpawnAmmoData->WeaponPickupAmmoMultiplierOverrideArray.Num(); i++)
				{
					auto& MultiplierData = SpawnAmmoData->WeaponPickupAmmoMultiplierOverrideArray.Get(i, FHelperGameplayTagToAmmoCountMultiplier::Size());

					if (WeaponTags.HasTag(MultiplierData.Tag))
					{
						Multiplier = MultiplierData.CountMultiplier.Evaluate((float)WorldLevel);
						break;
					}
				}

				float	SourceMultiplier = 1.f;

				if (Container)
				{
					FGameplayTagContainer SourceTags{};

					auto Interface3 = (IGameplayTagAssetInterface*)Container->GetInterface(IGameplayTagAssetInterface::StaticClass());
					if (Interface3)
					{
						auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface3->Vft[0x2];
						GetOwnedGameplayTags(Interface3, &SourceTags);
						//Interface->GetOwnedGameplayTags(&TargetTags);
					}

					for (int i = 0; i < SpawnAmmoData->SourceToAmmoMultiplierOverrideArray.Num(); i++)
					{
						auto& MultiplierData = SpawnAmmoData->SourceToAmmoMultiplierOverrideArray.Get(i, FHelperGameplayTagToAmmoCountMultiplier::Size());

						if (SourceTags.HasTag(MultiplierData.Tag))
						{
							SourceMultiplier = MultiplierData.CountMultiplier.Evaluate((float)WorldLevel);
							break;
						}
					}
					SourceTags.GameplayTags.Free();
					SourceTags.ParentTags.Free();
				}

				auto FinalCount = AmmoCount * Multiplier * SourceMultiplier;

				if (FinalCount > 0.f)
					AmmoEntry = AFortInventory::MakeItemEntry(AmmoDefinition, (int)FinalCount, AmmoDefinition->IsA(UFortWorldItemDefinition::StaticClass()) ? (AmmoDefinition->HasLootLevelData() ? std::clamp(GetLevel(AmmoDefinition->LootLevelData), AmmoDefinition->MinLevel, AmmoDefinition->MaxLevel > 0 ? AmmoDefinition->MaxLevel : 99999) : 0) : 0);

				AmmoTags.GameplayTags.Free();
				AmmoTags.ParentTags.Free();
				WeaponTags.GameplayTags.Free();
				WeaponTags.ParentTags.Free();
			}
		}
	}

	bool found = false;
	bool foundAmmo = false;
	for (auto& LootDrop : LootDrops)
	{
		if (/*(!AmmoDef || AmmoDef->DropCount) && */LootDrop->ItemDefinition == ItemDefinition && LootDrop->Count < ItemDefinition->GetMaxStackSize())
		{
			LootDrop->Count += LootPackage->Count;

			if (LootDrop->Count > ItemDefinition->GetMaxStackSize())
			{
				auto OGCount = LootDrop->Count;
				LootDrop->Count = ItemDefinition->GetMaxStackSize();

				//if (Inventory::GetQuickbar(LootDrop.ItemDefinition) == EFortQuickBars::Secondary)
				LootDrops.Add(AFortInventory::MakeItemEntry(ItemDefinition, OGCount - (int32)ItemDefinition->GetMaxStackSize(), ItemDefinition->IsA(UFortWorldItemDefinition::StaticClass()) ? (ItemDefinition->HasLootLevelData() ? std::clamp(GetLevel(ItemDefinition->LootLevelData), ItemDefinition->MinLevel, ItemDefinition->MaxLevel > 0 ? ItemDefinition->MaxLevel : 99999) : 0) : 0));
			}

			//if (Inventory::GetQuickbar(LootDrop.ItemDefinition) == EFortQuickBars::Secondary)
			found = true;
			break;
		}

		if (AmmoEntry && AmmoEntry->ItemDefinition && LootDrop->ItemDefinition == AmmoEntry->ItemDefinition)
		{
			LootDrop->Count += AmmoEntry->Count;

			if (LootDrop->Count > AmmoEntry->ItemDefinition->GetMaxStackSize())
			{
				auto OGCount = LootDrop->Count;
				LootDrop->Count = AmmoEntry->ItemDefinition->GetMaxStackSize();

				//if (!AFortInventory::IsPrimaryQuickbar(LootDrop->ItemDefinition))
				LootDrops.Add(AFortInventory::MakeItemEntry(AmmoEntry->ItemDefinition, OGCount - AmmoEntry->ItemDefinition->GetMaxStackSize(), AmmoEntry->ItemDefinition->IsA(UFortWorldItemDefinition::StaticClass()) ? (AmmoEntry->ItemDefinition->HasLootLevelData() ? std::clamp(GetLevel(AmmoEntry->ItemDefinition->LootLevelData), AmmoEntry->ItemDefinition->MinLevel, AmmoEntry->ItemDefinition->MaxLevel > 0 ? AmmoEntry->ItemDefinition->MaxLevel : 99999) : 0) : 0));
			}

			//if (Inventory::GetQuickbar(LootDrop.ItemDefinition) == EFortQuickBars::Secondary)
			free(AmmoEntry);
			foundAmmo = true;
		}
	}

	if (!found && LootPackage->Count > 0)
		LootDrops.Add(AFortInventory::MakeItemEntry(ItemDefinition, LootPackage->Count, ItemDefinition->IsA(UFortWorldItemDefinition::StaticClass()) ? (ItemDefinition->HasLootLevelData() ? std::clamp(GetLevel(ItemDefinition->LootLevelData), ItemDefinition->MinLevel, ItemDefinition->MaxLevel > 0 ? ItemDefinition->MaxLevel : 99999) : 0) : 0));

	if (!foundAmmo && AmmoEntry && LootPackage->Count > 0)
		LootDrops.Add(AmmoEntry);
	LPGroups.Free();
}

void UFortLootPackage::ChooseLootForContainer(TArray<FFortItemEntry*>& LootDrops, FName TierGroup, int LootTier, int WorldLevel, ABuildingContainer* Container)
{
	TArray<FFortLootTierData*> TierDataGroups{};
	
	for (auto const& Val : TierDataMap[TierGroup.ComparisonIndex])
		if (LootTier == -1 ? true : LootTier == Val->LootTier)
			TierDataGroups.Add(Val);

	auto LootTierData = PickWeighted(TierDataGroups, [](float Total)
		{ return ((float)rand() / 32767.f) * Total; });

	if (!LootTierData)
		return;
	
	//printf("Picked LootTierData %s\n", LootTierData->LootPackage.ToString().c_str());

	if (LootTierData->NumLootPackageDrops <= 0)
		return;

	//printf("Selecting %f loot drops from <unk>\n", LootTierData->NumLootPackageDrops);
	if (VersionInfo.FortniteVersion == 19.40)
	{
		auto& MinArr = LootTierData->LootPackageCategoryMinArray;

		if (MinArr.Num() > 1 && MinArr[1] == 0 && LootTierData->LootPackage.ToString().starts_with("WorldPKG.AthenaLoot.Weapon."))
		{
			MinArr[1] = 1;
			LootTierData->NumLootPackageDrops++;
		}
	}

	int DropCount;
	if (LootTierData->NumLootPackageDrops < 1.f)
		DropCount = 1;
	else
	{
		DropCount = (int)((LootTierData->NumLootPackageDrops * 2.f) - .5f) >> 1;

		float RemainderSomething = LootTierData->NumLootPackageDrops - (float)DropCount;

		if (RemainderSomething > 0.0000099999997f)
			DropCount += RemainderSomething >= ((float)rand() / 32767.f);
	}

	float AmountOfLootDrops = 0;
	float MinLootDrops = 0;

	std::unordered_map<int, int> NumMap;

	for (int i = 0; i < LootTierData->LootPackageCategoryMinArray.Num(); i++)
	{
		NumMap[i] = LootTierData->LootPackageCategoryMinArray[i];
		AmountOfLootDrops += LootTierData->LootPackageCategoryMinArray[i];
	}

	int SumWeights = 0;
	std::unordered_map<int, int> WeightMap;

	for (int i = 0; i < LootTierData->LootPackageCategoryWeightArray.Num(); i++)
	{
		if (LootTierData->LootPackageCategoryWeightArray[i] > 0 && (LootTierData->LootPackageCategoryMaxArray[i] < 0 || NumMap[i] < LootTierData->LootPackageCategoryMaxArray[i]))
		{
			WeightMap[i] = LootTierData->LootPackageCategoryWeightArray[i];
			SumWeights += LootTierData->LootPackageCategoryWeightArray[i];
		}
	}
	
	if (AmountOfLootDrops < DropCount)
		while (SumWeights > 0)
		{
			auto RandomValue = (float)rand() / 32767.f;
			auto RandomWeight = (int)std::floor(RandomValue * SumWeights);

			int Category = -1;
			for (auto& [DropCategory, Weight] : WeightMap)
			{
				if (RandomWeight <= Weight && RandomWeight <= LootTierData->NumLootPackageDrops)
				{
					Category = DropCategory;
					break;
				}

				RandomWeight -= Weight;
			}

			if (Category != -1)
			{
				AmountOfLootDrops++;
				NumMap[Category]++;

				if (LootTierData->LootPackageCategoryMaxArray[Category] >= 0 && NumMap[Category] >= LootTierData->LootPackageCategoryMaxArray[Category])
				{
					SumWeights -= LootTierData->LootPackageCategoryWeightArray[Category];
					WeightMap.erase(Category);
				}

				if (AmountOfLootDrops >= DropCount)
					break;
			}
		}

	/*if (AmountOfLootDrops < DropCount)
		while (SumWeights > 0)
		{
			AmountOfLootDrops++;

			if (AmountOfLootDrops >= DropCount)
			{
				//AmountOfLootDrops = AmountOfLootDrops;
				break;
			}

			SumWeights--;
		}

	if (!AmountOfLootDrops)
		return {};*/

	LootDrops.Reserve((int)DropCount);


	int SpawnedItems = 0;
	int CurrentCategory = 0;
	while (SpawnedItems < DropCount && CurrentCategory < NumMap.size())
	{
		for (int j = 0; j < NumMap[CurrentCategory]; j++)
			SetupLDSForPackage(LootDrops, LootTierData->LootPackage, CurrentCategory, TierGroup, WorldLevel);

		SpawnedItems += NumMap[CurrentCategory];
		CurrentCategory++;
	}
	TierDataGroups.Free();
}


bool UFortLootPackage::SpawnLootHook(ABuildingContainer* Container)
{
	if (Container->bAlreadySearched)
		return false;

	auto RealTierGroup = Container->SearchLootTierGroup;
	auto GameMode = ((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode);
	if (GameMode->HasRedirectAthenaLootTierGroups())
	{
		static auto RedirectAthenaLootTierGroupsOff = GameMode->GetOffset("RedirectAthenaLootTierGroups");

		if (VersionInfo.FortniteVersion >= 20)
		{
			auto& RedirectAthenaLootTierGroups = *(TMap<int32, int32>*)(__int64(GameMode) + RedirectAthenaLootTierGroupsOff);

			for (const auto& [OldTierGroup, RedirectedTierGroup] : RedirectAthenaLootTierGroups)
			{
				if (OldTierGroup == Container->SearchLootTierGroup.ComparisonIndex)
				{
					RealTierGroup.ComparisonIndex = RedirectedTierGroup;
					break;
				}
			}
		}
		else
		{
			auto& RedirectAthenaLootTierGroups = *(TMap<FName, FName>*)(__int64(GameMode) + RedirectAthenaLootTierGroupsOff);

			for (const auto& [OldTierGroup, RedirectedTierGroup] : RedirectAthenaLootTierGroups)
			{
				if (OldTierGroup == Container->SearchLootTierGroup)
				{
					RealTierGroup = RedirectedTierGroup;
					break;
				}
			}
		}
	}
	else
	{
		static auto Loot_Treasure = FName(L"Loot_Treasure");
		static auto Loot_Ammo = FName(L"Loot_Ammo");
		static auto Loot_AthenaTreasure = FName(L"Loot_AthenaTreasure");
		static auto Loot_AthenaAmmoLarge = FName(L"Loot_AthenaAmmoLarge");

		if (Container->SearchLootTierGroup == Loot_Treasure)
			RealTierGroup = Loot_AthenaTreasure;
		else if (Container->SearchLootTierGroup == Loot_Ammo)
			RealTierGroup = Loot_AthenaAmmoLarge;
	}

	TArray<FFortItemEntry*> LootDrops{};

	UFortLootPackage::ChooseLootForContainer(LootDrops, RealTierGroup, -1, GameMode->GameState->WorldLevel, Container);

	for (auto& LootDrop : LootDrops)
	{
		AFortInventory::SpawnPickup(Container, *LootDrop);
		free(LootDrop);
	}

	Container->bAlreadySearched = true;
	Container->OnRep_bAlreadySearched();
	Container->SearchBounceData.SearchAnimationCount++;
	Container->BounceContainer();
	//if (Container->bDestroyContainerOnSearch)
	//	Container->K2_DestroyActor();

	return true;
}


void UFortLootPackage::SpawnLoot(FName& TierGroup, FVector Loc)
{
	auto& RealTierGroup = TierGroup;

	auto GameMode = ((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode);
	if (GameMode->HasRedirectAthenaLootTierGroups())
	{
		for (const auto& [OldTierGroup, RedirectedTierGroup] : GameMode->RedirectAthenaLootTierGroups)
		{
			if (OldTierGroup == TierGroup)
			{
				RealTierGroup = RedirectedTierGroup;
				break;
			}
		}
	}
	else
	{
		static auto Loot_Treasure = FName(L"Loot_Treasure");
		static auto Loot_Ammo = FName(L"Loot_Ammo");
		static auto Loot_AthenaTreasure = FName(L"Loot_AthenaTreasure");
		static auto Loot_AthenaAmmoLarge = FName(L"Loot_AthenaAmmoLarge");

		if (TierGroup == Loot_Treasure)
			RealTierGroup = Loot_AthenaTreasure;
		else if (TierGroup == Loot_Ammo)
			RealTierGroup = Loot_AthenaAmmoLarge;
	}

	TArray<FFortItemEntry*> LootDrops{};

	UFortLootPackage::ChooseLootForContainer(LootDrops, RealTierGroup);

	for (auto& LootDrop : LootDrops)
	{
		AFortInventory::SpawnPickup(Loc, *LootDrop);
		free(LootDrop);
	}
}


bool ServerOnAttemptInteract(ABuildingContainer* BuildingContainer, AFortPlayerPawnAthena*)
{

	if (!BuildingContainer)
		return false;

	if (BuildingContainer->bAlreadySearched)
		return true;

	//SpawnLoot(BuildingContainer->SearchLootTierGroup, BuildingContainer->K2_GetActorLocation() + BuildingContainer->GetActorRightVector() * 70.f + FVector{ 0, 0, 50 });
	UFortLootPackage::SpawnLootHook(BuildingContainer);

	/*BuildingContainer->bAlreadySearched = true;
	BuildingContainer->OnRep_bAlreadySearched();
	BuildingContainer->SearchBounceData.SearchAnimationCount++;
	BuildingContainer->BounceContainer();*/

	return true;
}

using ApplyGameplayEffectSpecToOwnerExecFn =
	void(*)(UObject*, FFrame&, FActiveGameplayEffectHandle*);
static ApplyGameplayEffectSpecToOwnerExecFn
	ApplyGameplayEffectSpecToOwnerExecOG = nullptr;

static bool HasExactClassName(
	const UObject* Object, const FName& ExpectedClassName)
{
	return Object && Object->Class &&
		Object->Class->Name == ExpectedClassName;
}

static const UClass* ResolveGenericConsumeClass()
{
	static const UClass* GenericConsumeClass = nullptr;
	if (!GenericConsumeClass)
	{
		GenericConsumeClass = FindObject<UClass>(
			L"/Game/Athena/BuildingActors/ConsumableBGAs/Abilities/"
			L"GA_GenericConsume.GA_GenericConsume_C");
	}
	return GenericConsumeClass;
}

static const UClass* ResolveHopRockClass()
{
	static const UClass* HopRockClass = nullptr;
	if (!HopRockClass)
	{
		HopRockClass = FindObject<UClass>(
			L"/Game/Athena/Items/ForagedItems/LowGravity/"
			L"CBGA_LowGravity_UsingJump."
			L"CBGA_LowGravity_UsingJump_C");
	}
	return HopRockClass;
}

static bool IsActorBeingDestroyed(const AActor* Actor)
{
	return Actor && Actor->HasbActorIsBeingDestroyed() &&
		Actor->bActorIsBeingDestroyed;
}

static bool HasNearbyLiveActorOfClass(
	const UClass* ActorClass, const FVector& Location,
	double RadiusSquared, AActor** OutActor = nullptr)
{
	if (!ActorClass)
		return false;

	TArray<AActor*> Actors{};
	Utils::GetAll(ActorClass, Actors);

	AActor* FoundActor = nullptr;
	for (auto Actor : Actors)
	{
		if (!Actor || Actor->Class != ActorClass ||
			IsActorBeingDestroyed(Actor))
		{
			continue;
		}

		const auto Delta =
			Actor->K2_GetActorLocation() - Location;
		if (Delta.SizeSquared() <= RadiusSquared)
		{
			FoundActor = Actor;
			break;
		}
	}

	Actors.Free();
	if (OutActor)
		*OutActor = FoundActor;
	return FoundActor != nullptr;
}

static void DisableAndScheduleActorRemoval(AActor* Actor)
{
	if (!Actor || IsActorBeingDestroyed(Actor))
		return;

	// Give the replicated hidden/collision state a frame to reach clients
	// before UE closes the actor channel. This also prevents another consume
	// while the deferred destruction is pending.
	Actor->FlushNetDormancy();
	auto BuildingActor = (ABuildingActor*)Actor;
	if (BuildingActor->HasbAllowInteract())
		BuildingActor->bAllowInteract = false;
	Actor->SetActorHiddenInGame(true);

	auto SetActorEnableCollision =
		Actor->GetFunction("SetActorEnableCollision");
	if (SetActorEnableCollision)
	{
		bool bEnableCollision = false;
		Actor->Call<void>(
			SetActorEnableCollision, bEnableCollision);
	}
	else
	{
		static bool bLoggedMissingCollisionFunction = false;
		if (!bLoggedMissingCollisionFunction)
		{
			SDK::DbgLog(
				"[HopRock] SetActorEnableCollision missing; "
				"hidden/noninteractive removal continues\n");
			bLoggedMissingCollisionFunction = true;
		}
	}

	Actor->ForceNetUpdate();
	Actor->SetLifeSpan(0.25f);
}

static int32 ScheduleHopRockClusterRemoval(
	ABuildingGameplayActorConsumable* ConsumedHopRock,
	const UClass* HopRockClass)
{
	if (!ConsumedHopRock || !HopRockClass)
		return 0;

	// Multiple loot entries were being spawned on the same BGA spawner
	// transform. Remove only exact-class actors within ten Unreal units of the
	// consumed source, leaving legitimately separate nearby rocks untouched.
	constexpr double DuplicateRadiusSquared = 100.0;
	const auto ConsumedLocation =
		ConsumedHopRock->K2_GetActorLocation();

	TArray<AActor*> HopRocks{};
	Utils::GetAll(HopRockClass, HopRocks);

	int32 ScheduledCount = 0;
	bool bFoundConsumedActor = false;
	for (auto Actor : HopRocks)
	{
		if (!Actor || Actor->Class != HopRockClass)
			continue;

		if (Actor == ConsumedHopRock)
			bFoundConsumedActor = true;

		const auto Delta =
			Actor->K2_GetActorLocation() - ConsumedLocation;
		if (Delta.SizeSquared() > DuplicateRadiusSquared ||
			IsActorBeingDestroyed(Actor))
		{
			continue;
		}

		DisableAndScheduleActorRemoval(Actor);
		ScheduledCount++;
	}
	HopRocks.Free();

	// GetAllActorsOfClass can omit an actor already transitioning out of the
	// world. Still schedule the confirmed source if it was not in that list.
	if (!bFoundConsumedActor &&
		!IsActorBeingDestroyed(ConsumedHopRock))
	{
		DisableAndScheduleActorRemoval(ConsumedHopRock);
		ScheduledCount++;
	}

	SDK::DbgLog(
		"[HopRock] removal-scheduled source=%p authority=%d "
		"cluster=%d loc=(%.1f,%.1f,%.1f)\n",
		(void*)ConsumedHopRock,
		ConsumedHopRock->HasAuthority(),
		ScheduledCount,
		ConsumedLocation.X,
		ConsumedLocation.Y,
		ConsumedLocation.Z);
	return ScheduledCount;
}

static void HopRockApplyGameplayEffectSpecToOwner(
	UObject* Context, FFrame& Stack,
	FActiveGameplayEffectHandle* Result)
{
	if (!ApplyGameplayEffectSpecToOwnerExecOG)
		return;

	static const FName GenericConsumeClassName(
		L"GA_GenericConsume_C");
	static const FName HopRockClassName(
		L"CBGA_LowGravity_UsingJump_C");

	UObject* SourceObject = nullptr;
	const bool bHasGenericConsumeName =
		HasExactClassName(Context, GenericConsumeClassName);
	const auto GenericConsumeClass = bHasGenericConsumeName
		? ResolveGenericConsumeClass() : nullptr;
	const bool bIsGenericConsume =
		GenericConsumeClass &&
		Context->Class == GenericConsumeClass;
	if (bIsGenericConsume)
	{
		auto GetCurrentSourceObject =
			Context->GetFunction("GetCurrentSourceObject");
		if (GetCurrentSourceObject)
		{
			// Capture this before the native effect application returns to the
			// ability graph. GA_GenericConsume uses the live consumable actor as
			// its ability source object.
			SourceObject =
				Context->Call<UObject*>(GetCurrentSourceObject);
		}
	}

	// The original thunk consumes the opaque 4.20 effect-spec parameter and
	// writes the authoritative application result. Do not parse the stack here:
	// its layout differs between engine versions.
	ApplyGameplayEffectSpecToOwnerExecOG(
		Context, Stack, Result);

	const bool bHasHopRockName =
		HasExactClassName(SourceObject, HopRockClassName);
	const auto HopRockClass = bHasHopRockName
		? ResolveHopRockClass() : nullptr;
	const bool bIsHopRock = HopRockClass &&
		SourceObject->Class == HopRockClass;
	const bool bEffectApplied =
		Result && Result->bPassedFiltersAndWasExecuted;

	static int32 TraceCount = 0;
	if (bHasGenericConsumeName && TraceCount++ < 16)
	{
		SDK::DbgLog(
			"[HopRock] consume-result context-exact=%d "
			"source=%p source-name-match=%d source-exact=%d "
			"handle=%d applied=%d\n",
			bIsGenericConsume,
			(void*)SourceObject,
			bHasHopRockName,
			bIsHopRock,
			Result ? Result->Handle : -1,
			bEffectApplied);
	}

	if (!bIsHopRock)
		return;

	auto HopRock =
		(ABuildingGameplayActorConsumable*)SourceObject;
	const bool bAlreadyDestroying =
		HopRock->HasbActorIsBeingDestroyed() &&
		HopRock->bActorIsBeingDestroyed;

	// This is deliberately post-success. A press that is rejected, interrupted,
	// or filtered cannot remove the rock. Removal is scheduled after the
	// consume ability returns so its remaining bytecode can safely finish.
	if (bEffectApplied && !bAlreadyDestroying &&
		HopRock->HasAuthority())
	{
		ScheduleHopRockClusterRemoval(
			HopRock, HopRockClass);
	}
}

static void InstallHopRockConsumeHook()
{
	if (VersionInfo.FortniteVersion < 4.00 ||
		VersionInfo.FortniteVersion >= 5.00)
	{
		return;
	}

	auto GameplayAbilityClass =
		UFortGameplayAbility::StaticClass();
	auto GameplayAbilityDefault = GameplayAbilityClass
		? GameplayAbilityClass->GetDefaultObj() : nullptr;
	auto ApplyEffectFunction = GameplayAbilityDefault
		? GameplayAbilityDefault->GetFunction(
			"K2_ApplyGameplayEffectSpecToOwner")
		: nullptr;

	if (!ApplyEffectFunction)
	{
		SDK::DbgLog(
			"[HopRock] probe=v3 apply-hook missing "
			"class=%p default=%p\n",
			(void*)GameplayAbilityClass,
			(void*)GameplayAbilityDefault);
		return;
	}

	auto ExecBefore = ApplyEffectFunction->ExecFunction;
	if (ExecBefore ==
		(void*)HopRockApplyGameplayEffectSpecToOwner)
	{
		SDK::DbgLog(
			"[HopRock] probe=v3 apply-hook already-installed "
			"fn=%p\n",
			(void*)ApplyEffectFunction);
		return;
	}

	Utils::ExecHook(
		ApplyEffectFunction,
		HopRockApplyGameplayEffectSpecToOwner,
		ApplyGameplayEffectSpecToOwnerExecOG);

	SDK::DbgLog(
		"[HopRock] probe=v3 apply-hook fn=%p before=%p "
		"original=%p after=%p\n",
		(void*)ApplyEffectFunction,
		(void*)ExecBefore,
		(void*)ApplyGameplayEffectSpecToOwnerExecOG,
		(void*)ApplyEffectFunction->ExecFunction);
}

void UFortLootPackage::SpawnFloorLootForContainer(const UClass* ContainerType)
{
	TArray<ABuildingContainer*> Containers;
	Utils::GetAll<ABuildingContainer>(ContainerType, Containers);

	for (auto& BuildingContainer : Containers)
	{
		if (VersionInfo.FortniteVersion >= 11.00)
		{
			BuildingContainer->K2_DestroyActor();
		}
		//SpawnLootHook(BuildingContainer);
		else
		{
			SpawnLootHook(BuildingContainer);
			//SpawnLoot(BuildingContainer->SearchLootTierGroup, BuildingContainer->K2_GetActorLocation() + BuildingContainer->GetActorForwardVector() * BuildingContainer->LootSpawnLocation_Athena.X + BuildingContainer->GetActorRightVector() * BuildingContainer->LootSpawnLocation_Athena.Y + BuildingContainer->GetActorUpVector() * BuildingContainer->LootSpawnLocation_Athena.Z);

			if (VersionInfo.FortniteVersion > 3.3)
				BuildingContainer->K2_DestroyActor();
			else
			{
				BuildingContainer->bAlreadySearched = true;
				BuildingContainer->OnRep_bAlreadySearched();
			}
		}
	}

	Containers.Free();
}

struct FConsumableSpawnerHitResult
{
	uint8 OpaqueStorage[0x120]{};
};

struct FConsumableSpawnerTraceColor
{
	float R = 0.f;
	float G = 0.f;
	float B = 0.f;
	float A = 0.f;
};

static bool TryFindConsumableSpawnerSurface(
	ABGAConsumableSpawner* Spawner,
	FVector Start,
	FVector& OutImpactPoint)
{
	auto KismetSystemLibrary = UKismetSystemLibrary::GetDefaultObj();
	if (!KismetSystemLibrary)
		return false;

	auto LineTraceSingle =
		KismetSystemLibrary->GetFunction("LineTraceSingle");
	if (!LineTraceSingle)
		LineTraceSingle =
			KismetSystemLibrary->GetFunction("LineTraceSingle_NEW");
	if (!LineTraceSingle)
		return false;

	TArray<AActor*> ActorsToIgnore{};
	if (Spawner->HasAssociatedBuildingActor() &&
		Spawner->AssociatedBuildingActor)
	{
		ActorsToIgnore.Add(Spawner->AssociatedBuildingActor);
	}

	FConsumableSpawnerHitResult Hit{};
	const auto End = Start - FVector(0.f, 0.f, 100000.f);
	const bool bHit = KismetSystemLibrary->Call<bool>(
		LineTraceSingle, Spawner, Start, End, static_cast<uint8>(0), false,
		ActorsToIgnore, static_cast<uint8>(0), &Hit, true,
		FConsumableSpawnerTraceColor{},
		FConsumableSpawnerTraceColor{}, 0.f);
	ActorsToIgnore.Free();

	if (!bHit)
		return false;

	static int32 ImpactPointOffset = -2;
	if (ImpactPointOffset == -2)
	{
		auto HitResultStruct = FindStruct("HitResult");
		ImpactPointOffset = HitResultStruct
			? static_cast<int32>(
				HitResultStruct->GetOffset("ImpactPoint"))
			: -1;
	}

	if (ImpactPointOffset < 0 ||
		ImpactPointOffset + FVector::Size() >
			static_cast<int32>(sizeof(Hit.OpaqueStorage)))
	{
		return false;
	}

	auto& ImpactPoint = *(FVector*)(
		Hit.OpaqueStorage + ImpactPointOffset);
	if (!std::isfinite(static_cast<double>(ImpactPoint.X)) ||
		!std::isfinite(static_cast<double>(ImpactPoint.Y)) ||
		!std::isfinite(static_cast<double>(ImpactPoint.Z)) ||
		(ImpactPoint.IsZero() && !Start.IsZero()))
	{
		return false;
	}

	OutImpactPoint = ImpactPoint;
	return true;
}

bool UFortLootPackage::SpawnConsumableActor(ABGAConsumableSpawner* Spawner)
{
	auto World = UWorld::GetWorld();
	if (!World || !Spawner)
		return false;

	if (Spawner->HasAssociatedBuildingActor())
	{
		auto AssociatedBuilding = Spawner->AssociatedBuildingActor;
		if (AssociatedBuilding && AssociatedBuilding->HasbDestroyed() &&
			AssociatedBuilding->bDestroyed)
		{
			return false;
		}
	}

	TArray<FFortItemEntry*> LootDrops{};

	if (Spawner->HasSpawnLootTierGroup() &&
		Spawner->SpawnLootTierGroup.ComparisonIndex != 0)
	{
		UFortLootPackage::ChooseLootForContainer(
			LootDrops, Spawner->SpawnLootTierGroup);
	}

	AActor* SpawnedHopRockForSpawner = nullptr;
	auto SpawnEntry = [&](FFortItemEntry& Entry) -> bool
	{
		if (!Entry.ItemDefinition)
			return false;

		auto SpawnLocation = Spawner->K2_GetActorLocation();
		auto SpawnRotation = Spawner->K2_GetActorRotation();

		const bool bAlignToSurface =
			Spawner->HasbAlignSpawnedActorsToSurface() &&
			Spawner->bAlignSpawnedActorsToSurface;
		if (bAlignToSurface)
		{
			FVector GroundLocation{};
			if (TryFindConsumableSpawnerSurface(
				Spawner, SpawnLocation, GroundLocation))
				SpawnLocation = GroundLocation;
		}

		UClass* ConsumableActorClass = nullptr;
		auto WrapperClass =
			UBGAConsumableWrapperItemDefinition::StaticClass();
		if (WrapperClass && Entry.ItemDefinition->IsA(WrapperClass))
		{
			auto WrapperDefinition =
				(UBGAConsumableWrapperItemDefinition*)Entry.ItemDefinition;
			if (WrapperDefinition->HasConsumableClass())
				ConsumableActorClass =
					WrapperDefinition->ConsumableClass.Get();
		}

		if (ConsumableActorClass)
		{
			const auto HopRockClass = ResolveHopRockClass();
			const bool bIsSeasonFourHopRock =
				VersionInfo.FortniteVersion >= 4.00 &&
				VersionInfo.FortniteVersion < 5.00 &&
				HopRockClass &&
				ConsumableActorClass == HopRockClass;

			// A loot package can yield duplicate entries for one consumable
			// spawner. It previously created visually stacked actors: consuming
			// one revealed the identical actor underneath. Treat an exact-class
			// actor already on this transform as the spawner's resolved result.
			constexpr double DuplicateRadiusSquared = 100.0;
			AActor* ExistingActor = nullptr;
			if (bIsSeasonFourHopRock &&
				SpawnedHopRockForSpawner &&
				!IsActorBeingDestroyed(SpawnedHopRockForSpawner))
			{
				SDK::DbgLog(
					"[HopRock] duplicate-spawn-skipped "
					"spawner=%p existing=%p reason=same-spawner\n",
					(void*)Spawner,
					(void*)SpawnedHopRockForSpawner);
				return true;
			}
			if (bIsSeasonFourHopRock &&
				HasNearbyLiveActorOfClass(
				ConsumableActorClass, SpawnLocation,
				DuplicateRadiusSquared, &ExistingActor))
			{
				SDK::DbgLog(
					"[HopRock] duplicate-spawn-skipped "
					"spawner=%p existing=%p "
					"loc=(%.1f,%.1f,%.1f)\n",
					(void*)Spawner,
					(void*)ExistingActor,
					SpawnLocation.X,
					SpawnLocation.Y,
					SpawnLocation.Z);
				return true;
			}

			auto ConsumableDefault =
				(ABuildingGameplayActorConsumable*)
				ConsumableActorClass->GetDefaultObj();
			if (ConsumableDefault &&
				ConsumableDefault->HasbSpawnerCalculateRandomRotation() &&
				ConsumableDefault->bSpawnerCalculateRandomRotation)
			{
				SpawnRotation.Yaw = static_cast<float>(rand() % 360);
			}

			auto SpawnedActor = (AActor*)World->SpawnActor(
				ConsumableActorClass,
				FTransform(SpawnLocation, SpawnRotation));
			if (SpawnedActor && bIsSeasonFourHopRock)
			{
				SpawnedHopRockForSpawner = SpawnedActor;
				const auto ActualLocation =
					SpawnedActor->K2_GetActorLocation();
				SDK::DbgLog(
					"[HopRock] spawned actor=%p spawner=%p "
					"requested=(%.1f,%.1f,%.1f) "
					"actual=(%.1f,%.1f,%.1f)\n",
					(void*)SpawnedActor,
					(void*)Spawner,
					SpawnLocation.X,
					SpawnLocation.Y,
					SpawnLocation.Z,
					ActualLocation.X,
					ActualLocation.Y,
					ActualLocation.Z);
			}
			return SpawnedActor != nullptr;
		}

		return AFortInventory::SpawnPickup(
			SpawnLocation, Entry, EFortPickupSourceTypeFlag::GetOther(),
			EFortPickupSpawnSource::GetItemSpawner(), nullptr, -1,
			bAlignToSurface, true) != nullptr;
	};

	bool bSpawnedAny = false;

	if (LootDrops.Num() > 0)
	{
		for (auto LootDrop : LootDrops)
		{
			if (LootDrop)
				bSpawnedAny |= SpawnEntry(*LootDrop);
		}
	}
	else if (Spawner->HasConsumablesToSpawn())
	{
		auto& Consumables = Spawner->ConsumablesToSpawn;
		for (int32 Index = 0; Index < Consumables.Num(); ++Index)
		{
			auto& Entry =
				Consumables.Get(Index, FFortItemEntry::Size());
			bSpawnedAny |= SpawnEntry(Entry);
		}
	}

	for (auto LootDrop : LootDrops)
		free(LootDrop);
	LootDrops.Free();

	return bSpawnedAny;
}

void (*OnAuthorityRandomUpgradeAppliedOG)(ABuildingContainer*, FName&);
void OnAuthorityRandomUpgradeApplied(ABuildingContainer* Container, FName& UpgradeTierGroup)
{
	if (!Container->HasChosenRandomUpgrade()) // 15.10 what
		return OnAuthorityRandomUpgradeAppliedOG(Container, UpgradeTierGroup);

	auto ChosenRandomUpgrade = Container->ChosenRandomUpgrade;

	if (Container->HasAlternateMeshes())
	{
		if (ChosenRandomUpgrade < 0 || ChosenRandomUpgrade >= Container->AlternateMeshes.Num())
			return OnAuthorityRandomUpgradeAppliedOG(Container, UpgradeTierGroup);

		auto& AlternateMeshSet = Container->AlternateMeshes[ChosenRandomUpgrade];

		Container->ReplicatedLootTier = AlternateMeshSet.Tier;
		Container->OnRep_LootTier();
	}
	else
	{
		auto ClassData = Container->GetClassData();
		if (ChosenRandomUpgrade < 0 || ChosenRandomUpgrade >= ClassData->AlternateMeshes.Num())
			return OnAuthorityRandomUpgradeAppliedOG(Container, UpgradeTierGroup);

		auto& AlternateMeshSet = ClassData->AlternateMeshes[ChosenRandomUpgrade];

		Container->ReplicatedLootTier = AlternateMeshSet.Tier;
		Container->OnRep_LootTier();
	}

	return OnAuthorityRandomUpgradeAppliedOG(Container, UpgradeTierGroup);
}

void PostUpdate(ABuildingSMActor* BuildingSMActor)
{
	static auto Chest = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Containers/Tiered_Chest_6_Parent.Tiered_Chest_6_Parent_C");
	static auto AmmoCrate = FindObject<UClass>(L"/Game/Building/ActorBlueprints/Containers/Tiered_Short_Ammo_3_Parent.Tiered_Short_Ammo_3_Parent_C");

	if (auto Container = BuildingSMActor->Cast<ABuildingContainer>())
	{
		if (Container->IsA(Chest) || Container->IsA(AmmoCrate))
			return;

		if (!Container->bStartAlreadySearched_Athena)
		{
			Container->bAlreadySearched = true;
			Container->OnRep_bAlreadySearched();
		}
	}
}


bool bDidntFind = false;
void UFortLootPackage::Hook()
{
	// The legacy BuildingContainer string below has no code xref in 4.20, so
	// install the Hop Rock success hook independently before any early return.
	InstallHopRockConsumeHook();

	/*if (VersionInfo.FortniteVersion < 3)
	{
		auto PostUpdate_ = Memcury::Scanner::FindStringRef(L"ABuildingSMActor::PostUpdate() Building: %s, AltMeshIdx: %d", false, 0, VersionInfo.FortniteVersion >= 19).ScanFor({ 0x40, 0x53 }, false).Get();

		Utils::Hook(PostUpdate_, PostUpdate);
	}*/

	if (VersionInfo.FortniteVersion >= 11.00)
	{
		Utils::Hook(FindSpawnLoot(), SpawnLootHook);

		auto OnAuthorityRandomUpgradeAppliedAddr = FindFunctionCall(L"OnAuthorityRandomUpgradeApplied", std::vector<uint8_t>{ 0x48, 0x89, 0x5C });
		Utils::Hook(OnAuthorityRandomUpgradeAppliedAddr, OnAuthorityRandomUpgradeApplied, OnAuthorityRandomUpgradeAppliedOG);
		return;
	}
	else
	{
		auto ServerOnAttemptInteractRef = Memcury::Scanner::FindStringRef(L"ABuildingContainer::ServerOnAttemptInteract %s failed for %s");

		if (ServerOnAttemptInteractRef.Get())
		{
			auto UnderlyingCall = ServerOnAttemptInteractRef;
			UnderlyingCall.ScanFor({ 0x41, 0xFF }, false);

			if (UnderlyingCall.Get() != ServerOnAttemptInteractRef.Get())
			{
				auto VFTIndex = *(uint32*)(UnderlyingCall.Get() + 3);

				if (VFTIndex != 0)
				{
					Utils::Hook<ABuildingContainer>(VFTIndex / 8, ServerOnAttemptInteract);

					return;
				}
			}
		}
	}

	// if we cant find serveronattemptinteract
	bDidntFind = true;
	return;
}
