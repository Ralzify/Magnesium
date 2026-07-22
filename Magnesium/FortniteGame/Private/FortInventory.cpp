#include "pch.h"
#include "../Public/FortInventory.h"
#include "../Public/FortPlayerPawnAthena.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortKismetLibrary.h"
#include "../../Erbium/Public/Configuration.h"
#include "../Public/FortWeapon.h"
#include <ShlObj.h>

uint32_t OnItemInstanceAddedVft;

bool UFortWorldItemDefinition::ServerExecute(UFortItem* Item, AFortPlayerControllerAthena* Instigator) const
{
    if (!this || !Item || !Instigator)
        return false;

    const int32 ServerExecuteVft = FindWorldItemDefinitionServerExecuteVft();
    if (ServerExecuteVft < 0 || ServerExecuteVft >= 1024 || !Vft[ServerExecuteVft])
        return false;

    return ((bool(*)(const UFortWorldItemDefinition*, UFortItem*, AFortPlayerControllerAthena*))Vft[ServerExecuteVft])(
        this, Item, Instigator);
}

UFortWorldItem* AFortInventory::GiveItem(const UFortItemDefinition* Def, int Count, int LoadedAmmo, int Level, bool ShowPickupNoti, bool updateInventory, int PhantomReserveAmmo, TArray<FFortItemEntryStateValue> StateValues)
{
    if (!this || !Def || !Count)
        return nullptr;

    auto PlayerController = Owner ? Owner->Cast<AFortPlayerControllerAthena>() : nullptr;
    auto Gadget = Def->Cast<UFortGadgetItemDefinition>();

    // Exclusive Chapter 1 gadgets expect the backpack to be emptied before
    // their native ServerExecute runs. The Infinity Gauntlet sets this flag.
    if (VersionInfo.FortniteVersion >= 4.0 && VersionInfo.FortniteVersion <= 4.5 &&
        PlayerController && PlayerController->MyFortPawn && Gadget &&
        Gadget->HasbDropAllOnEquip() && Gadget->bDropAllOnEquip)
    {
        const FVector DropLocation = PlayerController->MyFortPawn->K2_GetActorLocation();
        for (int32 Index = Inventory.ReplicatedEntries.Num() - 1; Index >= 0; Index--)
        {
            auto& ExistingEntry = Inventory.ReplicatedEntries.Get(Index, FFortItemEntry::Size());
            auto ExistingDefinition = ExistingEntry.ItemDefinition;
            if (!ExistingDefinition)
                continue;

            const bool CanDrop = ExistingDefinition->HasbCanBeDropped()
                ? ExistingDefinition->bCanBeDropped
                : (ExistingDefinition->GetPickupComponent()
                    ? ExistingDefinition->GetPickupComponent()->bCanBeDroppedFromInventory
                    : false);
            if (!CanDrop)
                continue;

            const FGuid ExistingGuid = ExistingEntry.ItemGuid;
            AFortInventory::SpawnPickup(DropLocation, ExistingEntry, EFortPickupSourceTypeFlag::GetPlayer(),
                EFortPickupSpawnSource::GetUnset(), PlayerController->MyFortPawn);
            Remove(ExistingGuid);
        }
    }

    UFortWorldItem* Item = (UFortWorldItem*)Def->CreateTemporaryItemInstanceBP(Count, Level);
    if (!Item)
    {
        SDK::DbgLog("[Inventory] CreateTemporaryItemInstanceBP failed: Def=%p Count=%d Level=%d FN=%.2f\n",
            (void*)Def, Count, Level, VersionInfo.FortniteVersion);
        return nullptr;
    }

    Item->SetOwningControllerForTemporaryItem(Owner);
    if (Item->HasOwnerInventory())
        Item->OwnerInventory = this;
    else if (Item->HasOwnerInventoryWeak())
        Item->OwnerInventoryWeak = this;
    Item->ItemEntry.ParentInventory = this;
    Item->ItemEntry.LoadedAmmo = LoadedAmmo;
    if (Item->ItemEntry.HasPhantomReserveAmmo())
        Item->ItemEntry.PhantomReserveAmmo = PhantomReserveAmmo;
    if (auto WeaponDef = Def->Cast<UFortWeaponItemDefinition>())
        if (WeaponDef->HasWeaponModSlots() && FFortItemEntry::HasWeaponModSlots())
            Item->ItemEntry.WeaponModSlots = WeaponDef->WeaponModSlots;
    if (Item->ItemEntry.HasStateValues() && StateValues.Num() > 0)
    {
        auto NewData = FMemory::Malloc(FFortItemEntryStateValue::Size() * StateValues.Num());
        memcpy(NewData, StateValues.Data, FFortItemEntryStateValue::Size() * StateValues.Num());
        Item->ItemEntry.StateValues.NumElements = StateValues.Num();
        Item->ItemEntry.StateValues.MaxElements = StateValues.Max();
        Item->ItemEntry.StateValues.Data = (FFortItemEntryStateValue*)NewData;
    }

    if (Item->ItemEntry.ItemGuid.A == 0 && Item->ItemEntry.ItemGuid.B == 0 && Item->ItemEntry.ItemGuid.C == 0 && Item->ItemEntry.ItemGuid.D == 0)
    {
        CoCreateGuid((GUID*)&Item->ItemEntry.ItemGuid);

        if (FFortItemEntry::HasTrackerGuid() && Item->ItemEntry.TrackerGuid.A == 0 && Item->ItemEntry.TrackerGuid.B == 0 && Item->ItemEntry.TrackerGuid.C == 0 && Item->ItemEntry.TrackerGuid.D == 0)
            CoCreateGuid((GUID*)&Item->ItemEntry.TrackerGuid);
    }


    auto& repEntry = this->Inventory.ReplicatedEntries.Add(Item->ItemEntry, FFortItemEntry::Size());
    repEntry.bIsReplicatedCopy = true;
    this->Inventory.ItemInstances.Add(Item);

    /*if (Item->ItemEntry.ItemDefinition->bForceFocusWhenAdded)
    {
        ((AFortPlayerControllerAthena*)Owner)->ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);
        ((AFortPlayerControllerAthena*)Owner)->ClientEquipItem(Item->ItemEntry.ItemGuid, true);
    }*/

    if (VersionInfo.FortniteVersion <= 3.60)
    {
        auto PlayerController = (AFortPlayerControllerAthena*)Owner;
        auto PlayerState = PlayerController ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;
        bool bIsBotInventory = PlayerState && PlayerState->HasbIsABot() && PlayerState->bIsABot;

        if (!bIsBotInventory && PlayerController && PlayerController->QuickBars && (IsPrimaryQuickbar(Def) || Def->ItemType == EFortItemType::GetBuildingPiece() || Def->ItemType == EFortItemType::GetTrap() || Def->ItemType == EFortItemType::GetWeaponHarvest()))
        {
            PlayerController->QuickBars->ServerAddItemInternal(Item->ItemEntry.ItemGuid, !(IsPrimaryQuickbar(Def) || Def->ItemType == EFortItemType::GetWeaponHarvest()), -3);
        }
    }

    if (updateInventory)
    {
        //Update(&Item->ItemEntry);

        bRequiresLocalUpdate = true;
        bRequiresSaving = true;

        HandleInventoryLocalUpdate(); // calls UpdateItemInstances, the func we actually want

        repEntry.bIsDirty = false;
        Inventory.MarkItemDirty(repEntry);
        ForceNetUpdate();
        Item->ItemEntry.bIsDirty = true;
    }

    if (OnItemInstanceAddedVft && Owner)
        ((bool(*)(const UFortWorldItem*, const IInterface*)) Item->Vft[OnItemInstanceAddedVft])(Item, Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()));

    // The S4 gauntlet is force-focused when collected. Route that focus through
    // ServerExecuteInventoryItem so the native gadget execution path above is
    // used instead of merely equipping its backing weapon definition.
    if (VersionInfo.FortniteVersion >= 4.0 && VersionInfo.FortniteVersion <= 4.5 &&
        PlayerController && Gadget && Def->HasbForceFocusWhenAdded() && Def->bForceFocusWhenAdded)
    {
        PlayerController->ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);
        PlayerController->ClientEquipItem(Item->ItemEntry.ItemGuid, true);
    }

    return Item;
}

UFortWorldItem* AFortInventory::GiveItem(FFortItemEntry& entry, int Count, bool ShowPickupNoti, bool updateInventory)
{
    if (Count == -1)
        Count = entry.Count;

    return GiveItem(entry.ItemDefinition, Count, entry.LoadedAmmo, entry.Level, ShowPickupNoti, updateInventory, entry.HasPhantomReserveAmmo() ? entry.PhantomReserveAmmo : 0, entry.HasStateValues() ? entry.StateValues : TArray<FFortItemEntryStateValue>{});
}

void AFortInventory::SetRequiresUpdate()
{
    Inventory.MarkArrayDirty();
    bRequiresLocalUpdate = true;
    bRequiresSaving = true;
    HandleInventoryLocalUpdate();

    ForceNetUpdate();
}

void AFortInventory::Update(FFortItemEntry* Entry)
{
    if (!Entry)
        return SetRequiresUpdate();

    if (Entry->bIsReplicatedCopy)
    {
        Entry->bIsDirty = false;
        Inventory.MarkItemDirty(*Entry);
        SetRequiresUpdate();
        goto _out;
    }

    if (Entry->ItemGuid.A == 0 && Entry->ItemGuid.B == 0 && Entry->ItemGuid.C == 0 && Entry->ItemGuid.D == 0)
        goto _out;

    for (int i = 0; i < Inventory.ReplicatedEntries.Num(); i++)
    {
        auto& repEntry = Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

        if (repEntry.ItemGuid == Entry->ItemGuid)
        {
            repEntry = *Entry;
            repEntry.bIsDirty = false;
            Inventory.MarkItemDirty(repEntry);
            SetRequiresUpdate();
            break;
        }
    }
_out:
    return;
    /*bRequiresLocalUpdate = true;
    HandleInventoryLocalUpdate();

    return Entry ? Inventory.MarkItemDirty(*Entry) : Inventory.MarkArrayDirty();*/
}

uint64_t ClearAbility_;
void AFortInventory::RemoveWeaponAbilities(AActor* Weapon__Uncasted)
{
    auto Weapon = (AFortWeapon*)Weapon__Uncasted;
    auto ClearAbility = (void(*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle&)) ClearAbility_;
    auto PlayerController = (AFortPlayerControllerAthena*)((AFortPlayerPawnAthena*)Weapon->Instigator)->Controller;
    if (!PlayerController)
        return;
    auto AbilitySystemComponent = PlayerController->PlayerState->AbilitySystemComponent;

    if (Weapon->PrimaryAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->PrimaryAbilitySpecHandle);
        Weapon->PrimaryAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->SecondaryAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->SecondaryAbilitySpecHandle);
        Weapon->SecondaryAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->ReloadAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->ReloadAbilitySpecHandle);
        Weapon->ReloadAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->ImpactAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->ImpactAbilitySpecHandle);
        Weapon->ImpactAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->EquippedAbilityHandles.Num())
    {
        for (int i = 0; i < Weapon->EquippedAbilityHandles.Num(); i++)
        {
            auto& Handle = Weapon->EquippedAbilityHandles.Get(i, FGameplayAbilitySpecHandle::Size());

            if (IsBadReadPtr(&Handle)) // what
                continue;

            if (Handle.Handle != -1)
            {
                ClearAbility(AbilitySystemComponent, Handle);
            }
        }
        Weapon->EquippedAbilityHandles.ResetNum();
    }

    /*if (Weapon->WeaponData->EquippedAbilitySet)
    {
        bool bRemoved = false;
        for (auto& [Key, Value] : *(TMap<FGuid, FFortAbilitySetHandle>*)& PlayerController->AppliedInGameModifierAbilitySetHandles)
            if (Key == Weapon->ItemEntryGuid)
            {
                UFortKismetLibrary::UnequipFortAbilitySet(Value);
                bRemoved = true;
                break;
            }

        if (bRemoved)
            PlayerController->ClientRemoveItemAbilitySet(Weapon->WeaponData->EquippedAbilitySet, Weapon->ItemEntryGuid);
    }*/
    if (Weapon->EquippedAbilitySetHandles.Num())
    {
        for (int i = 0; i < Weapon->EquippedAbilitySetHandles.Num(); i++)
        {
            auto& Handle = Weapon->EquippedAbilitySetHandles.Get(i, FFortAbilitySetHandle::Size());

            UFortKismetLibrary::UnequipFortAbilitySet(Handle);
        }

        PlayerController->AppliedInGameModifierAbilitySetHandles.Reset();
        Weapon->EquippedAbilitySetHandles.ResetNum();
    }
}

void AFortInventory::Remove(FGuid Guid)
{
    auto ItemEntryIdx = Inventory.ReplicatedEntries.SearchIndex([&](FFortItemEntry& entry)
        { return entry.ItemGuid == Guid; }, FFortItemEntry::Size());
    if (ItemEntryIdx == -1)
        return;

    auto EntryDef = Inventory.ReplicatedEntries.Get(ItemEntryIdx, FFortItemEntry::Size()).ItemDefinition;

    auto ItemInstanceIdx = Inventory.ItemInstances.SearchIndex([&](UFortWorldItem* entry)
        { return entry->ItemEntry.ItemGuid == Guid; });
    auto ItemInstanceResult = Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
        { return entry->ItemEntry.ItemGuid == Guid; });

    // Save the object before mutating either replicated array. Search returns
    // storage owned by the array, which is invalid after Remove.
    auto Instance = ItemInstanceResult ? *ItemInstanceResult : nullptr;


    if (ItemEntryIdx != -1)
        Inventory.ReplicatedEntries.Remove(ItemEntryIdx, FFortItemEntry::Size());
    if (ItemInstanceIdx != -1)
        Inventory.ItemInstances.Remove(ItemInstanceIdx);

    auto PlayerController = (AFortPlayerControllerAthena*)Owner;
    if (PlayerController && PlayerController->QuickBars && EntryDef)
    {
        auto& QuickBar = IsPrimaryQuickbar(EntryDef) ? PlayerController->QuickBars->PrimaryQuickBar : PlayerController->QuickBars->SecondaryQuickBar;
        int i = 0;
        for (i = 0; i < QuickBar.Slots.Num(); i++)
        {
            auto& Slot = QuickBar.Slots.Get(i, FQuickBarSlot::Size());

            for (auto& Item : Slot.Items)
                if (Item == Guid)
                    goto _Out;
        }
        goto _Skip;
    _Out:
        PlayerController->QuickBars->EmptySlot(!IsPrimaryQuickbar(EntryDef), i);
        PlayerController->QuickBars->ServerRemoveItemInternal(Guid, false, true);
        for (int i = 0; i < PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
        {
            auto& Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

            if (Entry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest())
            {
                PlayerController->ServerExecuteInventoryItem(Entry.ItemGuid);
                PlayerController->QuickBars->ServerActivateSlotInternal(0, 0, 0.f, true);
            }
        }
    }

_Skip:

    bRequiresLocalUpdate = true;
    bRequiresSaving = true;

    HandleInventoryLocalUpdate(); // calls UpdateItemInstances, the func we actually want

    Inventory.MarkArrayDirty();
    ForceNetUpdate();

    if (OnItemInstanceAddedVft && Instance && Instance->ItemEntry.ItemDefinition)
    {
        ((bool(*)(const UFortWorldItem*, const IInterface*, uint32_t)) Instance->Vft[OnItemInstanceAddedVft + 1])(Instance, Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()), Instance->ItemEntry.Count);
        //((bool(*)(const UFortItemDefinition*, const IInterface*, UFortWorldItem*)) Instance->ItemEntry.ItemDefinition->Vft[OnItemInstanceAddedVft + 1])(Instance->ItemEntry.ItemDefinition, Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()), Instance);
    }
    //HandleInventoryLocalUpdate();
    //Update(nullptr);
}

FFortRangedWeaponStats* AFortInventory::GetStats(const UFortWeaponItemDefinition* Def)
{
    if (!Def || !Def->WeaponStatHandle.DataTable)
        return nullptr;

    auto Val = Def->WeaponStatHandle.DataTable->RowMap.Search([Def](FName& Key, uint8_t* Value)
        {
            return Def->WeaponStatHandle.RowName == Key && Value;
        });

    return Val ? *(FFortRangedWeaponStats**)Val : nullptr;
}

FFortRangedWeaponStats* AFortInventory::CloneStats(const UFortWeaponItemDefinition* Def)
{
    auto BaseStats = GetStats(Def);
    if (!BaseStats)
        return nullptr;

    auto NewStats = (FFortRangedWeaponStats*)FMemory::Malloc(sizeof(FFortRangedWeaponStats));
    FMemory::Memcpy(NewStats, BaseStats, sizeof(FFortRangedWeaponStats));

    return NewStats;
}

FFortItemEntry* AFortInventory::MakeItemEntry(const UFortItemDefinition* ItemDefinition, int32 Count, int32 Level)
{
    auto ItemEntry = (FFortItemEntry*)malloc(FFortItemEntry::Size());
    memset((PBYTE)ItemEntry, 0, FFortItemEntry::Size());

    ItemEntry->MostRecentArrayReplicationKey = -1;
    ItemEntry->ReplicationID = -1;
    ItemEntry->ReplicationKey = -1;

    auto ItemDef = ItemDefinition;
    if (auto MultiWorldItemDef = ItemDef->Cast<UFortWorldMultiItemDefinition>())
    {
        ItemDef = MultiWorldItemDef->ItemInfos.Get(rand() % MultiWorldItemDef->ItemInfos.Num(), FFortWorldMultiItemInfo::Size()).ItemDefinition;
    }
    ItemEntry->ItemDefinition = ItemDef;
    ItemEntry->Count = Count;
    ItemEntry->Durability = 1.f;
    ItemEntry->GameplayAbilitySpecHandle = FGameplayAbilitySpecHandle(-1);
    ItemEntry->ParentInventory.ObjectIndex = -1;
    ItemEntry->Level = Level;
    if (auto Weapon = ItemDef->IsA<UFortGadgetItemDefinition>() ? (UFortWeaponItemDefinition*)((UFortGadgetItemDefinition*)ItemDef)->GetWeaponItemDefinition() : ItemDef->Cast<UFortWeaponItemDefinition>())
    {
        auto Stats = GetStats(Weapon);
        if (Stats)
        {
            ItemEntry->LoadedAmmo = Stats->ClipSize;

            if (Weapon->HasbUsesPhantomReserveAmmo() && Weapon->bUsesPhantomReserveAmmo)
                ItemEntry->PhantomReserveAmmo = (Stats->InitialClips - 1) * Stats->ClipSize;
        }
    }
    if (auto WeaponDef = ItemDef->Cast<UFortWeaponItemDefinition>())
        if (WeaponDef->HasWeaponModSlots() && FFortItemEntry::HasWeaponModSlots())
            ItemEntry->WeaponModSlots = WeaponDef->WeaponModSlots;
    if (ItemEntry->HasPickupVariantIndex())
        ItemEntry->PickupVariantIndex = -1;
    if (ItemEntry->HasItemVariantDataMappingIndex())
        ItemEntry->ItemVariantDataMappingIndex = -1;
    if (ItemEntry->HasOrderIndex())
        ItemEntry->OrderIndex = -1;

    return ItemEntry;
}

uint64_t SetPickupItems;
AFortPickupAthena* AFortInventory::SpawnPickup(FVector Loc, FFortItemEntry& Entry, long long SourceTypeFlag, long long SpawnSource, AFortPlayerPawnAthena* Pawn, int OverrideCount, bool Toss, bool RandomRotation, bool bCombine, const UClass* OverrideClass, FVector FinalLoc)
{
    if (!&Entry)
        return nullptr;
    AFortPickupAthena* NewPickup = UWorld::SpawnActor<AFortPickupAthena>(OverrideClass ? OverrideClass : AFortPickupAthena::StaticClass(), Loc, {});
    if (!NewPickup)
        return nullptr;

    if (NewPickup->HasbRandomRotation())
        NewPickup->bRandomRotation = RandomRotation;
    if (Entry.Level != -1)
        NewPickup->PrimaryPickupItemEntry.Level = Entry.Level;
    NewPickup->PrimaryPickupItemEntry.ItemDefinition = Entry.ItemDefinition;
    NewPickup->PrimaryPickupItemEntry.LoadedAmmo = Entry.LoadedAmmo;
    NewPickup->PrimaryPickupItemEntry.Count = OverrideCount != -1 ? OverrideCount : Entry.Count;
    static auto HasPhantomReserveAmmo = Entry.HasPhantomReserveAmmo();
    if (HasPhantomReserveAmmo)
        NewPickup->PrimaryPickupItemEntry.PhantomReserveAmmo = Entry.PhantomReserveAmmo;

    if (SetPickupItems)
    {
        TArray<FFortItemEntry> a{};
        if (VersionInfo.FortniteVersion >= 16)
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, uint8_t, bool, uint8_t)) SetPickupItems)(NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, (uint8_t)EFortPickupSourceTypeFlag::GetContainer(), false, (uint8_t)EFortPickupSpawnSource::GetChest());
        else
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, bool)) SetPickupItems)(NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, false);
    }
    else
        NewPickup->OnRep_PrimaryPickupItemEntry();
    //NewPickup->OnRep_PrimaryPickupItemEntry();
    NewPickup->PawnWhoDroppedPickup = Pawn;

    auto FinalLocation = Loc;
    
    if (FinalLoc.X || FinalLoc.Y || FinalLoc.Z)
        FinalLocation = FinalLoc;
    NewPickup->TossPickup(FinalLocation, Pawn, -1, Toss, true, (uint8)SourceTypeFlag, (uint8)SpawnSource);

    if (SpawnSource != -1)
        NewPickup->bTossedFromContainer = SpawnSource == EFortPickupSpawnSource::GetChest() || SpawnSource == EFortPickupSpawnSource::GetAmmoBox();
    if (NewPickup->bTossedFromContainer)
        NewPickup->OnRep_TossedFromContainer();

    return NewPickup;
}

AFortPickupAthena* AFortInventory::SpawnPickup(FVector Loc, const UFortItemDefinition* ItemDefinition, int Count, int LoadedAmmo, long long SourceTypeFlag, long long SpawnSource, AFortPlayerPawnAthena* Pawn, bool Toss, bool bRandomRotation, const UClass* OverrideClass)
{
    auto ItemEntry = MakeItemEntry(ItemDefinition, Count, -1);
    if (LoadedAmmo != -1) // -1 keeps the clip/phantom ammo MakeItemEntry derived from the weapon's stats
        ItemEntry->LoadedAmmo = LoadedAmmo;

    auto Pickup = SpawnPickup(Loc, *ItemEntry, SourceTypeFlag, SpawnSource, Pawn, -1, Toss, true, bRandomRotation, OverrideClass);
    free(ItemEntry);
    return Pickup;
}


AFortPickupAthena* AFortInventory::SpawnPickup(ABuildingContainer* Container, FFortItemEntry& Entry, AFortPlayerPawnAthena* Pawn, int OverrideCount)
{
    if (!&Entry)
        return nullptr;

    auto ContainerLoc = Container->K2_GetActorLocation();
    auto SpawnLocation = Container->HasLootSpawnLocation_Athena() ? Container->LootSpawnLocation_Athena : Container->LootSpawnLocation;
    if (VersionInfo.FortniteVersion >= 24)
    {
        auto& ProperSpawnLoc = *(FVector3f*)(__int64(Container) + Container->LootSpawnLocation_Athena__Offset);

        SpawnLocation.X = ProperSpawnLoc.X;
        SpawnLocation.Y = ProperSpawnLoc.Y;
        SpawnLocation.Z = ProperSpawnLoc.Z;
    }
    auto Loc = ContainerLoc + (Container->GetActorForwardVector() * SpawnLocation.X) + (Container->GetActorRightVector() * SpawnLocation.Y) + (Container->GetActorUpVector() * SpawnLocation.Z);
    AFortPickupAthena* NewPickup = UWorld::SpawnActor<AFortPickupAthena>(Loc, {});

    if (!NewPickup)
        return nullptr;

    if (NewPickup->HasbRandomRotation())
        NewPickup->bRandomRotation = true;
    NewPickup->PrimaryPickupItemEntry.Level = Entry.Level;
    NewPickup->PrimaryPickupItemEntry.ItemDefinition = Entry.ItemDefinition;
    NewPickup->PrimaryPickupItemEntry.LoadedAmmo = Entry.LoadedAmmo;
    NewPickup->PrimaryPickupItemEntry.Count = OverrideCount != -1 ? OverrideCount : Entry.Count;
    static auto HasPhantomReserveAmmo = Entry.HasPhantomReserveAmmo();
    if (HasPhantomReserveAmmo)
        NewPickup->PrimaryPickupItemEntry.PhantomReserveAmmo = Entry.PhantomReserveAmmo;

    if (SetPickupItems)
    {
        TArray<FFortItemEntry> a{};
        if (VersionInfo.FortniteVersion >= 16)
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, uint8_t, bool, uint8_t)) SetPickupItems)(NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, (uint8_t)EFortPickupSourceTypeFlag::GetContainer(), false, (uint8_t)EFortPickupSpawnSource::GetChest());
        else
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, bool)) SetPickupItems)(NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, false);
    }
    else
        NewPickup->OnRep_PrimaryPickupItemEntry();
    //NewPickup->OnRep_PrimaryPickupItemEntry();

    NewPickup->PawnWhoDroppedPickup = Pawn;


    //auto bFloorLoot = Container->IsA<ATiered_Athena_FloorLoot_01_C>() || Container->IsA<ATiered_Athena_FloorLoot_Warmup_C>();
    //UFortKismetLibrary::TossPickupFromContainer(UWorld::GetWorld(), Container, NewPickup, 1, 0, Container->LootTossConeHalfAngle_Athena, Container->LootTossDirection_Athena, Container->LootTossSpeed_Athena, false);
    static auto tpfcPtr = UFortKismetLibrary::GetDefaultObj()->GetFunction("TossPickupFromContainer");
    if (tpfcPtr)
    {
        if (!UFortKismetLibrary::TossPickupFromContainer__Ptr)
            UFortKismetLibrary::TossPickupFromContainer__Ptr = tpfcPtr;

        UFortKismetLibrary::TossPickupFromContainer(UWorld::GetWorld(), Container, NewPickup, 10, (int32)std::clamp((float)rand() * 0.0003357036f, 0.f, 10.f), Container->LootTossConeHalfAngle_Athena, Container->LootTossDirection_Athena, Container->LootTossSpeed_Athena, Container->bForceHidePickupMinimapIndicator);
    }
    else
    {
        auto FinalLoc = Loc + (Container->GetActorForwardVector() * Container->LootFinalLocation.X) + (Container->GetActorRightVector() * Container->LootFinalLocation.Y) + (Container->GetActorUpVector() * Container->LootFinalLocation.Z);

        NewPickup->TossPickup(Loc, Pawn, -1, true, true, EFortPickupSourceTypeFlag::GetContainer(), EFortPickupSpawnSource::GetChest());
    }

    NewPickup->bTossedFromContainer = true;
    NewPickup->OnRep_TossedFromContainer();

    return NewPickup;
}


bool AFortInventory::IsPrimaryQuickbar(const UFortItemDefinition* ItemDefinition)
{
    return
        ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest() ||
        ItemDefinition->ItemType == EFortItemType::GetWorldResource() ||
        ItemDefinition->ItemType == EFortItemType::GetAmmo() ||
        ItemDefinition->ItemType == EFortItemType::GetTrap() ||
        ItemDefinition->ItemType == EFortItemType::GetBuildingPiece() ||
        ItemDefinition->ItemType == EFortItemType::GetEditTool() ||
        ItemDefinition->ItemType == EFortItemType::GetIngredient() ||
        (ItemDefinition->HasbForceIntoOverflow() && ItemDefinition->bForceIntoOverflow)
        ? false : true;
}


void AFortInventory::UpdateEntry(FFortItemEntry& Entry)
{
    if (!this)
        return; // wtf 3.5


    /*auto ent = Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
        { return item.ItemGuid == Entry.ItemGuid; }, FFortItemEntry::Size());
    if (ent)
        *ent = Entry;*/

        /*auto ent2 = Inventory.ItemInstances.Search([&](UFortWorldItem* item)
            { return item->ItemEntry.ItemGuid == Entry.ItemGuid; });
        if (ent2)
            (*ent2)->ItemEntry = Entry;*/
            //memcpy((PBYTE)&(*ent)->ItemEntry, (const PBYTE)&Entry, FFortItemEntry::Size());

    Update(&Entry);
}

bool RemoveInventoryItem(IInterface* Interface, FGuid& ItemGuid, int Count, bool bForceRemoval)
{
    if (FConfiguration::bInfiniteAmmo)
        return true;

    static auto InterfaceOffset = FindClass("FortPlayerController")->GetSuper()->GetPropertiesSize() + (VersionInfo.EngineVersion >= 4.27 ? 16 : 8);
    auto PlayerController = (AFortPlayerControllerAthena*)(__int64(Interface) - InterfaceOffset);

    if (PlayerController->bInfiniteAmmo)
        return true;

    auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
        { return entry->ItemEntry.ItemGuid == ItemGuid; });
    auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
        { return entry.ItemGuid == ItemGuid; }, FFortItemEntry::Size());

    if (ItemP)
    {

        auto Item = *ItemP;

        /*for (int i = 0; i < itemEntry->StateValues.Num(); i++)
        {
            auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

            if (StateValue.StateType != 2)
                continue;

            StateValue.IntValue = 0;
        }*/


        itemEntry->Count -= max(Count, 0);
        if (Count < 0 || itemEntry->Count <= 0 || bForceRemoval)
        {
            if (Item->ItemEntry.ItemDefinition->HasbPersistInInventoryWhenFinalStackEmpty() && Item->ItemEntry.ItemDefinition->bPersistInInventoryWhenFinalStackEmpty && Count > 0)
            {
                auto OtherStack = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
                    { return item.ItemDefinition == Item->ItemEntry.ItemDefinition && item.ItemGuid != ItemGuid; }, FFortItemEntry::Size());

                if (!OtherStack)
                {
                    /*for (int i = 0; i < itemEntry->StateValues.Num(); i++)
                    {
                        auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

                        if (StateValue.StateType != 2)
                            continue;

                        StateValue.IntValue = 0;
                        break;
                    }*/

                    Item->ItemEntry.Count = itemEntry->Count;
                    PlayerController->WorldInventory->UpdateEntry(*itemEntry);
                    Item->ItemEntry.bIsDirty = true;
                }
                else
                    PlayerController->WorldInventory->Remove(ItemGuid);
            }
            else
                PlayerController->WorldInventory->Remove(ItemGuid);
        }
        else
        {
            /*for (int i = 0; i < itemEntry->StateValues.Num(); i++)
            {
                auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

                if (StateValue.StateType != 2)
                    continue;

                StateValue.IntValue = 0;
                break;
            }*/

            Item->ItemEntry.Count = itemEntry->Count;
            PlayerController->WorldInventory->UpdateEntry(*itemEntry);
            Item->ItemEntry.bIsDirty = true;
        }

        return true;
    }

    return false;
}

void SetLoadedAmmo(UFortWorldItem* Item, int LoadedAmmo)
{
    auto PlayerController = (AFortPlayerControllerAthena*)Item->GetOwningController();
    //PlayerController->WorldInventory->UpdateEntry(Item->ItemEntry);
    auto repEnt = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
        { return item.ItemGuid == Item->ItemEntry.ItemGuid; }, FFortItemEntry::Size());

    repEnt->LoadedAmmo = LoadedAmmo;
    Item->ItemEntry.LoadedAmmo = LoadedAmmo;
    PlayerController->WorldInventory->UpdateEntry(*repEnt);
    Item->ItemEntry.bIsDirty = true;
}

void SetPhantomReserveAmmo(UFortWorldItem* Item, unsigned int PhantomReserveAmmo)
{
    auto PlayerController = (AFortPlayerControllerAthena*)Item->GetOwningController();
    //PlayerController->WorldInventory->UpdateEntry(Item->ItemEntry);
    auto repEnt = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
        { return item.ItemGuid == Item->ItemEntry.ItemGuid; }, FFortItemEntry::Size());

    repEnt->PhantomReserveAmmo = PhantomReserveAmmo;
    Item->ItemEntry.PhantomReserveAmmo = PhantomReserveAmmo;

    PlayerController->WorldInventory->UpdateEntry(*repEnt);
    Item->ItemEntry.bIsDirty = true;
}


void SpawnPickup_(UObject* Object, FFrame& Stack, AFortPickupAthena** Ret)
{
    UFortItemDefinition* ItemDefinition;
    int32 NumberToSpawn;
    AFortPlayerPawnAthena* TriggeringPawn;
    FVector Position;
    FVector Direction;
    Stack.StepCompiledIn(&ItemDefinition);
    Stack.StepCompiledIn(&NumberToSpawn);
    Stack.StepCompiledIn(&TriggeringPawn);
    Stack.StepCompiledIn(&Position);
    Stack.StepCompiledIn(&Direction);
    Stack.IncrementCode();

    *Ret = AFortInventory::SpawnPickup(Position, ItemDefinition, NumberToSpawn, -1, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetSupplyDrop());
}

void RemoveInventoryStateValue()
{
    printf("Sup\n");
}

void SetInventoryStateValue()
{
    printf("Sup2\n");
}

void AFortInventory::PostLoadHook()
{
    SDK::DbgLog("  [FI] 0 pre-FindSetPickupItems\n");
    SetPickupItems = FindSetPickupItems();
    SDK::DbgLog("  [FI] 0a pre-FindOnItemInstanceAddedVft\n");
    OnItemInstanceAddedVft = FindOnItemInstanceAddedVft();
    SDK::DbgLog("  [FI] 0b pre-FindClearAbility\n");
    ClearAbility_ = FindClearAbility();
    SDK::DbgLog("  [FI] 1 finds done\n");

    Utils::Hook(FindRemoveInventoryItem(), RemoveInventoryItem);
    SDK::DbgLog("  [FI] 2 RemoveInventoryItem hooked\n");
    // need to see if these are used
    //Utils::Hook(FindRemoveInventoryStateValue(), RemoveInventoryStateValue);
    //Utils::Hook(FindSetInventoryStateValue(), SetInventoryStateValue);

    auto SetOwningInventory = Memcury::Scanner::FindPattern("48 85 D2 74 ? 80 BA ? ? ? ? ? 75 ? 48 89 91").Get();
    if (!SetOwningInventory)
        SetOwningInventory = Memcury::Scanner::FindPattern("48 83 EC ? 48 85 D2 74 ? 80 BA ? ? ? ? ? 75 ? 48 81 C1").Get();
    if (!SetOwningInventory)
        SetOwningInventory = Memcury::Scanner::FindPattern("48 85 D2 74 ? 48 89 5C 24 ? 57 48 83 EC ? 48 8B F9 48 8B DA 48 8B CA E8 ? ? ? ? 83 F8 ? 75").Get();

    if (SetOwningInventory)
    {
        auto WorldItemVft = UFortWorldItem::GetDefaultObj()->Vft;
        int SetOwningInventoryIdx = 0;

        for (int i = 0; i < 0x200; i++)
        {
            if (WorldItemVft[i] == (void*)SetOwningInventory)
            {
                SetOwningInventoryIdx = i;
                break;
            }
        }

        if (SetOwningInventoryIdx)
        {
            auto HasPhantomReserveAmmo = FFortItemEntry::HasPhantomReserveAmmo();

            Utils::Hook<UFortWorldItem>(uint32(SetOwningInventoryIdx - (HasPhantomReserveAmmo ? (VersionInfo.EngineVersion < 4.27 ? 2 : 3) : 1)), SetLoadedAmmo);
            if (HasPhantomReserveAmmo)
                Utils::Hook<UFortWorldItem>(uint32(SetOwningInventoryIdx - (VersionInfo.EngineVersion < 4.27 ? 1 : 2)), SetPhantomReserveAmmo);
        }
    }

    SDK::DbgLog("  [FI] 3 SetOwningInventory block done\n");
    if (auto sd = DefaultObjImpl("FortAthenaSupplyDrop"))
        Utils::ExecHook(sd->GetFunction("SpawnPickup"), SpawnPickup_);
    SDK::DbgLog("  [FI] 4 PostLoadHook complete\n");
}
