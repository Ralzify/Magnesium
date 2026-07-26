#include "pch.h"
#include "../Public/FortInventory.h"
#include "../Public/FortPlayerPawnAthena.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortKismetLibrary.h"
#include "../../Erbium/Public/Configuration.h"
#include "../Public/FortWeapon.h"
#include "../Public/FortWeaponMods.h"
#include <ShlObj.h>
#include <cmath>
#include <vector>

uint32_t OnItemInstanceAddedVft;

namespace
{
    struct FRegeneratingInventoryItem
    {
        TWeakObjectPtr<AFortPlayerControllerAthena> Owner;
        TWeakObjectPtr<UFortAmmoItemDefinition> AmmoDefinition;
        FGuid ItemGuid{};
        int32 MaxCount = 0;
        double CooldownSeconds = 0.0;
        double NextRefillTime = 0.0;
    };

    struct FRechargingWeaponAmmo
    {
        TWeakObjectPtr<AFortPlayerControllerAthena> Owner;
        TWeakObjectPtr<UFortWeaponItemDefinition> WeaponDefinition;
        FGuid ItemGuid{};
        int32 MaxLoadedAmmo = 0;
        int32 RechargeAmount = 0;
        int32 LastObservedLoadedAmmo = 0;
        double RechargeIntervalSeconds = 0.0;
        double NextRefillTime = 0.0;
    };

    std::vector<FRegeneratingInventoryItem> RegeneratingInventoryItems;
    std::vector<FRechargingWeaponAmmo> RechargingWeaponAmmo;

    constexpr size_t MaxTrackedRechargingWeapons = 128;
    constexpr double NativeRechargeGraceSeconds = 0.10;

    bool AreGuidsEqual(const FGuid& Left, const FGuid& Right)
    {
        return Left.A == Right.A &&
            Left.B == Right.B &&
            Left.C == Right.C &&
            Left.D == Right.D;
    }

    bool IsSameRegenItem(
        const FRegeneratingInventoryItem& State,
        const AFortPlayerControllerAthena* Owner,
        const FGuid& ItemGuid)
    {
        return State.Owner.Get() == Owner &&
            AreGuidsEqual(State.ItemGuid, ItemGuid);
    }

    void RemoveRegenItemAt(size_t Index)
    {
        if (Index + 1 != RegeneratingInventoryItems.size())
        {
            RegeneratingInventoryItems[Index] =
                RegeneratingInventoryItems.back();
        }
        RegeneratingInventoryItems.pop_back();
    }

    void RemoveRechargingWeaponAt(size_t Index)
    {
        if (Index + 1 != RechargingWeaponAmmo.size())
        {
            RechargingWeaponAmmo[Index] =
                RechargingWeaponAmmo.back();
        }
        RechargingWeaponAmmo.pop_back();
    }

    bool IsNitroFistsDefinition(
        const UFortWeaponItemDefinition* WeaponDefinition)
    {
        if (!WeaponDefinition ||
            VersionInfo.FortniteVersion < 30.0 ||
            VersionInfo.FortniteVersion >= 31.0)
        {
            return false;
        }

        const auto DefinitionName =
            WeaponDefinition->Name.ToString();
        return DefinitionName.rfind(
            "WID_Moonflax_NitroGauntlet", 0) == 0;
    }

    bool NotifyNitroFistsRechargeStarted(
        AFortPlayerControllerAthena* Owner,
        const FGuid& ItemGuid,
        double ServerStartTime)
    {
        if (!Owner || !std::isfinite(ServerStartTime))
            return false;

        auto RechargeComponentClass =
            FindClass("FortControllerComponent_RechargeWeapons");
        auto RechargeComponent =
            RechargeComponentClass
                ? Owner->GetComponentByClass(
                    RechargeComponentClass)
                : nullptr;
        auto ClientStartedFunction =
            RechargeComponent
                ? RechargeComponent->GetFunction(
                    "ClientItemStartedRecharging")
                : nullptr;
        if (!ClientStartedFunction)
            return false;

        // This native client RPC seeds the same server-time countdown used
        // by WBP_NitroGautletReticle's GetRemainingCooldownTimer path. The
        // client component also keeps a pending-GUID map, so this remains
        // valid while the equipped weapon is being constructed.
        RechargeComponent->Call<void>(
            ClientStartedFunction,
            ItemGuid,
            static_cast<float>(ServerStartTime));
        return true;
    }

    bool SyncEquippedNitroFistsAmmo(
        AFortPlayerControllerAthena* Owner,
        UFortWeaponItemDefinition* WeaponDefinition,
        const FGuid& ItemGuid,
        int32 NewLoadedAmmo)
    {
        if (!Owner || !WeaponDefinition ||
            !Owner->HasMyFortPawn() || !Owner->MyFortPawn ||
            !Owner->MyFortPawn->HasCurrentWeapon())
        {
            return false;
        }

        auto WeaponActor = Owner->MyFortPawn->CurrentWeapon;
        auto FortWeaponClass = AFortWeapon::StaticClass();
        if (!WeaponActor || !FortWeaponClass ||
            !WeaponActor->IsA(FortWeaponClass))
        {
            return false;
        }

        auto Weapon = static_cast<AFortWeapon*>(WeaponActor);
        if (!Weapon->HasItemEntryGuid() ||
            !AreGuidsEqual(Weapon->ItemEntryGuid, ItemGuid) ||
            (Weapon->HasWeaponData() &&
                Weapon->WeaponData != WeaponDefinition) ||
            !Weapon->HasAmmoCount())
        {
            return false;
        }

        const int32 OldAmmoCount = Weapon->AmmoCount;
        if (OldAmmoCount == NewLoadedAmmo)
            return true;

        // The equipped actor owns the authoritative/live magazine used by
        // Nitro abilities. Updating only FFortItemEntry explains why the
        // restored charge became usable after re-equipping: equip copied
        // the entry back into this field. Keep both representations in step
        // and drive the normal rep-notify/delegate path for HUD listeners.
        Weapon->AmmoCount = NewLoadedAmmo;
        if (auto OnRepAmmoCount =
            Weapon->GetFunction("OnRep_AmmoCount"))
        {
            Weapon->Call<void>(
                OnRepAmmoCount, OldAmmoCount);
        }
        Weapon->ForceNetUpdate();
        return true;
    }

    bool ResolveNitroFistsRechargeSettings(
        UFortWeaponItemDefinition* WeaponDefinition,
        int32 ItemLevel,
        int32& MaxLoadedAmmo,
        int32& RechargeAmount,
        double& RechargeIntervalSeconds)
    {
        if (!IsNitroFistsDefinition(WeaponDefinition))
            return false;

        const auto DefinitionName =
            WeaponDefinition->Name.ToString();
        // Do not reinterpret a gauntlet stat row as ranged-weapon stats.
        // FN30 defines four charges on the regular fists and five on the
        // mythic variant.
        MaxLoadedAmmo =
            DefinitionName.find("_Mythic") !=
                std::string::npos
            ? 5
            : 4;

        float RechargeQuantityValue = 0.0f;
        if (WeaponDefinition->HasWeaponRechargeAmmoQuantity())
        {
            RechargeQuantityValue =
                WeaponDefinition->WeaponRechargeAmmoQuantity.Evaluate(
                    static_cast<float>(max(ItemLevel, 1)));
        }
        RechargeAmount =
            static_cast<int32>(std::round(RechargeQuantityValue));

        float RechargeRateValue = 0.0f;
        if (WeaponDefinition->HasWeaponRechargeAmmoRate())
        {
            RechargeRateValue =
                WeaponDefinition->WeaponRechargeAmmoRate.Evaluate(
                    static_cast<float>(max(ItemLevel, 1)));
        }
        RechargeIntervalSeconds =
            static_cast<double>(RechargeRateValue);

        // These are the authoritative FN30 curve-table defaults:
        // Moonflax.NitroGauntlets.RechargeQuantity = 1 and both
        // RechargeRate rows = 8. The fallback is limited to the exact
        // Nitro Fists definitions in case a server-only asset load cannot
        // evaluate the scalable-float curve.
        if (RechargeAmount <= 0 || RechargeAmount > MaxLoadedAmmo)
            RechargeAmount = 1;
        if (!std::isfinite(RechargeIntervalSeconds) ||
            RechargeIntervalSeconds <= 0.0 ||
            RechargeIntervalSeconds > 300.0)
        {
            RechargeIntervalSeconds = 8.0;
        }

        return MaxLoadedAmmo > 0 &&
            RechargeAmount > 0;
    }

    void ObserveRechargingWeaponAmmo(
        AFortPlayerControllerAthena* Owner,
        UFortWeaponItemDefinition* WeaponDefinition,
        const FGuid& ItemGuid,
        int32 ItemLevel,
        int32 PreviousLoadedAmmo,
        int32 NewLoadedAmmo)
    {
        if (!Owner || !Owner->WorldInventory ||
            !WeaponDefinition)
        {
            return;
        }

        int32 MaxLoadedAmmo = 0;
        int32 RechargeAmount = 0;
        double RechargeIntervalSeconds = 0.0;
        if (!ResolveNitroFistsRechargeSettings(
                WeaponDefinition,
                ItemLevel,
                MaxLoadedAmmo,
                RechargeAmount,
                RechargeIntervalSeconds))
        {
            return;
        }

        auto World = UWorld::GetWorld();
        if (!World)
            return;

        const double NowSeconds =
            UGameplayStatics::GetTimeSeconds(World);
        for (size_t Index = 0;
            Index < RechargingWeaponAmmo.size();
            ++Index)
        {
            auto& State = RechargingWeaponAmmo[Index];
            if (State.Owner.Get() != Owner ||
                !AreGuidsEqual(State.ItemGuid, ItemGuid))
            {
                continue;
            }

            State.WeaponDefinition =
                TWeakObjectPtr<UFortWeaponItemDefinition>(
                    WeaponDefinition);
            State.MaxLoadedAmmo = MaxLoadedAmmo;
            State.RechargeAmount = RechargeAmount;
            State.RechargeIntervalSeconds =
                RechargeIntervalSeconds;
            State.LastObservedLoadedAmmo =
                std::clamp(
                    NewLoadedAmmo, 0, MaxLoadedAmmo);
            bool bStartedRechargeCycle = false;

            // A native recharge is an increase. Give its controller
            // component a complete interval before the watchdog can add
            // another charge. Further consumption does not reset an
            // already-running interval.
            if (NewLoadedAmmo >= MaxLoadedAmmo)
            {
                State.NextRefillTime = 0.0;
            }
            else if (NewLoadedAmmo > PreviousLoadedAmmo)
            {
                State.NextRefillTime =
                    NowSeconds +
                    RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
            else if (NewLoadedAmmo < PreviousLoadedAmmo &&
                State.NextRefillTime <= 0.0)
            {
                State.NextRefillTime =
                    NowSeconds +
                    RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
            if (bStartedRechargeCycle)
            {
                NotifyNitroFistsRechargeStarted(
                    Owner, ItemGuid, NowSeconds);
            }
            return;
        }

        if (RechargingWeaponAmmo.size() >=
                MaxTrackedRechargingWeapons)
        {
            return;
        }

        FRechargingWeaponAmmo State{};
        State.Owner =
            TWeakObjectPtr<AFortPlayerControllerAthena>(Owner);
        State.WeaponDefinition =
            TWeakObjectPtr<UFortWeaponItemDefinition>(
                WeaponDefinition);
        State.ItemGuid = ItemGuid;
        State.MaxLoadedAmmo = MaxLoadedAmmo;
        State.RechargeAmount = RechargeAmount;
        State.LastObservedLoadedAmmo =
            std::clamp(
                NewLoadedAmmo, 0, MaxLoadedAmmo);
        State.RechargeIntervalSeconds =
            RechargeIntervalSeconds;
        if (NewLoadedAmmo < MaxLoadedAmmo)
        {
            State.NextRefillTime =
                NowSeconds +
                RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
        }
        RechargingWeaponAmmo.push_back(State);

        const bool bClientTimerStarted =
            NewLoadedAmmo < MaxLoadedAmmo &&
            NotifyNitroFistsRechargeStarted(
                Owner, ItemGuid, NowSeconds);

        auto RechargeComponentClass =
            FindClass("FortControllerComponent_RechargeWeapons");
        auto RechargeComponent =
            RechargeComponentClass
                ? Owner->GetComponentByClass(
                    RechargeComponentClass)
                : nullptr;
        SDK::DbgLog(
            "[WeaponRecharge] registered definition=%s ammo=%d/%d "
            "amount=%d interval=%.2f nativeComponent=%s\n",
            WeaponDefinition->Name.ToString().c_str(),
            NewLoadedAmmo,
            MaxLoadedAmmo,
            RechargeAmount,
            RechargeIntervalSeconds,
            RechargeComponent ? "present" : "missing");
        if (NewLoadedAmmo < MaxLoadedAmmo)
        {
            SDK::DbgLog(
                "[WeaponRecharge] client-timer definition=%s "
                "started=%d serverStart=%.3f\n",
                WeaponDefinition->Name.ToString().c_str(),
                bClientTimerStarted ? 1 : 0,
                NowSeconds);
        }
    }

    void BroadcastWorldItemAmmoChanged(UFortWorldItem* Item)
    {
        if (!Item)
            return;

        static auto BroadcastFunction =
            Item->GetFunction("BroadcastOnItemChanged");
        if (BroadcastFunction)
        {
            Item->Call<void>(
                BroadcastFunction,
                false,
                true,
                false,
                false);
        }
    }

    void ScheduleRegeneratingInventoryItem(
        AFortPlayerControllerAthena* Owner,
        UFortAmmoItemDefinition* AmmoDefinition,
        const FGuid& ItemGuid,
        int32 MaxCount,
        double CooldownSeconds)
    {
        if (!Owner || !Owner->WorldInventory || !AmmoDefinition ||
            MaxCount <= 0 || !std::isfinite(CooldownSeconds) ||
            CooldownSeconds <= 0.0 || CooldownSeconds > 3600.0)
        {
            return;
        }

        auto ReplicatedEntry =
            Owner->WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, ItemGuid) &&
                        Candidate.ItemDefinition == AmmoDefinition;
                },
                FFortItemEntry::Size());
        if (!ReplicatedEntry || ReplicatedEntry->Count >= MaxCount)
            return;

        for (auto& State : RegeneratingInventoryItems)
        {
            if (!IsSameRegenItem(State, Owner, ItemGuid))
                continue;

            State.AmmoDefinition =
                TWeakObjectPtr<UFortAmmoItemDefinition>(AmmoDefinition);
            State.MaxCount = max(State.MaxCount, MaxCount);
            State.CooldownSeconds = CooldownSeconds;
            return;
        }

        auto World = UWorld::GetWorld();
        if (!World)
            return;

        FRegeneratingInventoryItem State{};
        State.Owner =
            TWeakObjectPtr<AFortPlayerControllerAthena>(Owner);
        State.AmmoDefinition =
            TWeakObjectPtr<UFortAmmoItemDefinition>(AmmoDefinition);
        State.ItemGuid = ItemGuid;
        State.MaxCount = MaxCount;
        State.CooldownSeconds = CooldownSeconds;
        State.NextRefillTime =
            UGameplayStatics::GetTimeSeconds(World) + CooldownSeconds;
        RegeneratingInventoryItems.push_back(State);

        SDK::DbgLog(
            "[ItemRegen] scheduled definition=%s count=%d max=%d cooldown=%.2f\n",
            AmmoDefinition->Name.ToString().c_str(),
            ReplicatedEntry->Count,
            MaxCount,
            CooldownSeconds);
    }
}

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

UFortWorldItem* AFortInventory::GiveItem(const UFortItemDefinition* Def, int Count, int LoadedAmmo, int Level, bool ShowPickupNoti, bool updateInventory, int PhantomReserveAmmo, TArray<FFortItemEntryStateValue> StateValues, bool bNotifyItemInstanceAdded)
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
        FFortWeaponMods::CopyDefinitionSlotsToEntry(
            WeaponDef, Item->ItemEntry);
    const bool bHasWeaponModSlots =
        FFortWeaponMods::HasEntrySlots(Item->ItemEntry);
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


    auto& AddedReplicatedEntry =
        this->Inventory.ReplicatedEntries.Add(
            Item->ItemEntry, FFortItemEntry::Size());
    auto* ReplicatedEntry = &AddedReplicatedEntry;
    ReplicatedEntry->bIsReplicatedCopy = true;
    if (bHasWeaponModSlots)
    {
        FFortWeaponMods::CopyEntrySlots(
            Item->ItemEntry, *ReplicatedEntry);
    }
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

        // Native inventory work can move the fast-array allocation. Never
        // retain the Add() reference across HandleInventoryLocalUpdate.
        ReplicatedEntry = Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Candidate)
            {
                return Candidate.ItemGuid ==
                    Item->ItemEntry.ItemGuid;
            },
            FFortItemEntry::Size());
        if (ReplicatedEntry)
        {
            ReplicatedEntry->bIsDirty = false;
            Inventory.MarkItemDirty(*ReplicatedEntry);
        }
        ForceNetUpdate();
        Item->ItemEntry.bIsDirty = true;
    }

    // HandleInventoryLocalUpdate may copy the replicated array header back to
    // the item instance. Detach it so bench changes cannot mutate both entries.
    if (bHasWeaponModSlots && ReplicatedEntry)
    {
        FFortWeaponMods::CopyEntrySlots(
            *ReplicatedEntry, Item->ItemEntry);
    }

    if (bNotifyItemInstanceAdded && OnItemInstanceAddedVft && Owner)
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

    auto RechargingWeaponDefinition =
        Item->ItemEntry.ItemDefinition
            ? Item->ItemEntry.ItemDefinition->Cast<
                UFortWeaponItemDefinition>()
            : nullptr;
    if (PlayerController &&
        IsNitroFistsDefinition(RechargingWeaponDefinition))
    {
        // Register at grant time as well as at the loaded-ammo setter.
        // This keeps the repair event-bound and still detects FN30 paths
        // that mutate the replicated entry without calling that setter.
        ObserveRechargingWeaponAmmo(
            PlayerController,
            RechargingWeaponDefinition,
            Item->ItemEntry.ItemGuid,
            Item->ItemEntry.Level,
            Item->ItemEntry.LoadedAmmo,
            Item->ItemEntry.LoadedAmmo);
    }

    return Item;
}

UFortWorldItem* AFortInventory::GiveItem(FFortItemEntry& entry, int Count, bool ShowPickupNoti, bool updateInventory)
{
    if (Count == -1)
        Count = entry.Count;

    // Preserve the original inventory/callback/force-focus ordering outside
    // Chapter 5. Deferred notification is needed only while copying the
    // attachment slots that native item listeners consume.
    if (!FFortWeaponMods::HasEntrySlots(entry))
    {
        return GiveItem(
            entry.ItemDefinition,
            Count,
            entry.LoadedAmmo,
            entry.Level,
            ShowPickupNoti,
            updateInventory,
            entry.HasPhantomReserveAmmo()
                ? entry.PhantomReserveAmmo
                : 0,
            entry.HasStateValues()
                ? entry.StateValues
                : TArray<FFortItemEntryStateValue>{});
    }

    auto Item = GiveItem(
        entry.ItemDefinition,
        Count,
        entry.LoadedAmmo,
        entry.Level,
        ShowPickupNoti,
        false,
        entry.HasPhantomReserveAmmo() ? entry.PhantomReserveAmmo : 0,
        entry.HasStateValues()
            ? entry.StateValues
            : TArray<FFortItemEntryStateValue>{},
        false);
    if (!Item)
        return nullptr;

    auto ReplicatedEntry = Inventory.ReplicatedEntries.Search(
        [&](FFortItemEntry& Candidate)
        {
            return Candidate.ItemGuid == Item->ItemEntry.ItemGuid;
        },
        FFortItemEntry::Size());

    FFortWeaponMods::CopyEntrySlots(entry, Item->ItemEntry);
    if (ReplicatedEntry)
        FFortWeaponMods::CopyEntrySlots(entry, *ReplicatedEntry);

    if (updateInventory)
    {
        bRequiresLocalUpdate = true;
        bRequiresSaving = true;
        HandleInventoryLocalUpdate();

        ReplicatedEntry = Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Candidate)
            {
                return Candidate.ItemGuid == Item->ItemEntry.ItemGuid;
            },
            FFortItemEntry::Size());
        if (ReplicatedEntry)
        {
            ReplicatedEntry->bIsDirty = false;
            Inventory.MarkItemDirty(*ReplicatedEntry);
            FFortWeaponMods::CopyEntrySlots(
                *ReplicatedEntry, Item->ItemEntry);
        }

        ForceNetUpdate();
        Item->ItemEntry.bIsDirty = true;
    }

    // Pickup/custom entries must expose their source attachment set before
    // inventory listeners initialize the item's abilities and equipped state.
    // The definition overload deliberately deferred this one notification.
    if (OnItemInstanceAddedVft && Owner)
        ((bool(*)(const UFortWorldItem*, const IInterface*)) Item->Vft[OnItemInstanceAddedVft])(Item, Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()));

    return Item;
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
        FGuid UpdatedGuid = Entry->ItemGuid;
        Entry->bIsDirty = false;
        Inventory.MarkItemDirty(*Entry);
        SetRequiresUpdate();

        auto UpdatedReplicatedEntry = Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Candidate)
            {
                return Candidate.ItemGuid == UpdatedGuid;
            },
            FFortItemEntry::Size());
        auto ItemInstance = Inventory.ItemInstances.Search(
            [&](UFortWorldItem* Candidate)
            {
                return Candidate &&
                    Candidate->ItemEntry.ItemGuid == UpdatedGuid;
            });
        if (UpdatedReplicatedEntry && ItemInstance && *ItemInstance)
        {
            FFortWeaponMods::CopyEntrySlots(
                *UpdatedReplicatedEntry,
                (*ItemInstance)->ItemEntry);
        }
        goto _out;
    }

    if (Entry->ItemGuid.A == 0 && Entry->ItemGuid.B == 0 && Entry->ItemGuid.C == 0 && Entry->ItemGuid.D == 0)
        goto _out;

    for (int i = 0; i < Inventory.ReplicatedEntries.Num(); i++)
    {
        auto& repEntry = Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

        if (repEntry.ItemGuid == Entry->ItemGuid)
        {
            FGuid UpdatedGuid = Entry->ItemGuid;
            TArray<void*> PreviousWeaponModSlots{};
            const bool bPreserveWeaponModAllocation =
                FFortWeaponMods::IsSupported() &&
                FFortItemEntry::HasWeaponModSlots();
            if (bPreserveWeaponModAllocation)
            {
                PreviousWeaponModSlots =
                    repEntry.WeaponModSlots;
            }

            // Preserve the destination's owned nested buffer across the
            // outer entry assignment. CopyEntrySlots can then replace and
            // release it safely instead of losing the allocation header.
            repEntry = *Entry;
            if (bPreserveWeaponModAllocation)
            {
                repEntry.WeaponModSlots =
                    PreviousWeaponModSlots;
            }
            FFortWeaponMods::CopyEntrySlots(*Entry, repEntry);
            repEntry.bIsDirty = false;
            Inventory.MarkItemDirty(repEntry);
            SetRequiresUpdate();

            auto UpdatedReplicatedEntry = Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return Candidate.ItemGuid == UpdatedGuid;
                },
                FFortItemEntry::Size());
            if (UpdatedReplicatedEntry)
            {
                FFortWeaponMods::CopyEntrySlots(
                    *UpdatedReplicatedEntry, *Entry);
            }
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

    auto& RemovedEntry = Inventory.ReplicatedEntries.Get(
        ItemEntryIdx, FFortItemEntry::Size());
    auto EntryDef = RemovedEntry.ItemDefinition;

    auto ItemInstanceIdx = Inventory.ItemInstances.SearchIndex([&](UFortWorldItem* entry)
        { return entry && entry->ItemEntry.ItemGuid == Guid; });
    auto ItemInstanceResult = Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
        { return entry && entry->ItemEntry.ItemGuid == Guid; });

    // Save the object before mutating either replicated array. Search returns
    // storage owned by the array, which is invalid after Remove.
    auto Instance = ItemInstanceResult ? *ItemInstanceResult : nullptr;

    // TArray::Remove performs a raw element shift and does not destruct nested
    // arrays. Release the attachment buffers explicitly. If a native inventory
    // update ever left the two entries sharing a header, clear one owner first
    // so the allocation is released exactly once.
    if (FFortWeaponMods::IsSupported() &&
        FFortItemEntry::HasWeaponModSlots())
    {
        if (Instance &&
            RemovedEntry.WeaponModSlots.Data &&
            RemovedEntry.WeaponModSlots.Data ==
                Instance->ItemEntry.WeaponModSlots.Data)
        {
            Instance->ItemEntry.WeaponModSlots.Data = nullptr;
            Instance->ItemEntry.WeaponModSlots.NumElements = 0;
            Instance->ItemEntry.WeaponModSlots.MaxElements = 0;
        }

        FFortWeaponMods::FreeEntrySlots(RemovedEntry);
        if (Instance)
            FFortWeaponMods::FreeEntrySlots(Instance->ItemEntry);
    }

    if (ItemEntryIdx != -1)
        Inventory.ReplicatedEntries.Remove(ItemEntryIdx, FFortItemEntry::Size());
    if (ItemInstanceIdx != -1)
        Inventory.ItemInstances.Remove(ItemInstanceIdx);

    auto PlayerController = (AFortPlayerControllerAthena*)Owner;
    // 7.40 removed FortPlayerController::QuickBars and moved to the client-side
    // quickbar model. Reading DEFINE_PROP(QuickBars) when it is absent resolves
    // to offset -1 and produces a bogus pointer (PlayerController - 1). Keep the
    // legacy actor bookkeeping only on builds that expose it; newer builds are
    // updated by the replicated inventory path below.
    AFortQuickBars* QuickBars = nullptr;
    if (VersionInfo.FortniteVersion < 7.40 && PlayerController &&
        PlayerController->HasQuickBars())
    {
        QuickBars = PlayerController->QuickBars;
    }

    if (QuickBars && EntryDef)
    {
        auto& QuickBar = IsPrimaryQuickbar(EntryDef) ? QuickBars->PrimaryQuickBar : QuickBars->SecondaryQuickBar;
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
        QuickBars->EmptySlot(!IsPrimaryQuickbar(EntryDef), i);
        QuickBars->ServerRemoveItemInternal(Guid, false, true);
        for (int i = 0; i < PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
        {
            auto& Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

            if (Entry.ItemDefinition && Entry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest())
            {
                PlayerController->ServerExecuteInventoryItem(Entry.ItemGuid);
                QuickBars->ServerActivateSlotInternal(0, 0, 0.f, true);
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

int32 AFortInventory::RemoveItem(
    FGuid Guid,
    int32 Count,
    bool bKeepFinalStackEmpty)
{
    auto ItemEntry =
        Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Entry)
            {
                return Entry.ItemGuid == Guid;
            },
            FFortItemEntry::Size());
    if (!ItemEntry || Count == 0)
        return 0;

    const bool bRemoveAll = Count < 0;
    const int32 ExistingCount = max(ItemEntry->Count, 0);
    const int32 RemovedCount =
        bRemoveAll ? ExistingCount : min(ExistingCount, Count);

    // Remove zero/negative terminal stacks too. They are invalid unless the
    // definition explicitly asked the caller to preserve the final empty
    // stack.
    if (bRemoveAll ||
        (!bKeepFinalStackEmpty &&
            (ExistingCount <= 0 || Count >= ExistingCount)))
    {
        Remove(Guid);
        return RemovedCount;
    }

    int32 NewCount =
        bKeepFinalStackEmpty && Count >= ExistingCount
            ? 0
            : ExistingCount - RemovedCount;
    ItemEntry->Count = NewCount;

    auto ItemInstance =
        Inventory.ItemInstances.Search(
            [&](UFortWorldItem* Item)
            {
                return Item &&
                    Item->ItemEntry.ItemGuid == Guid;
            });
    if (ItemInstance && *ItemInstance)
    {
        (*ItemInstance)->ItemEntry.Count = NewCount;
        (*ItemInstance)->ItemEntry.bIsDirty = true;
    }

    UpdateEntry(*ItemEntry);
    return RemovedCount;
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
    {
        if (FFortWeaponMods::IsSupported() &&
            WeaponDef->HasWeaponModSlots() &&
            FFortItemEntry::HasWeaponModSlots())
        {
            // MakeItemEntry returns a short-lived, caller-owned entry. Keep a
            // read-only view here; every durable destination (pickup or
            // inventory entry) deep-copies it before this temporary is freed.
            ItemEntry->WeaponModSlots = WeaponDef->WeaponModSlots;
        }
    }
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

    bool bAllowRandomMods = false;
    if (FFortWeaponMods::IsSupported())
    {
        bAllowRandomMods =
            SourceTypeFlag != EFortPickupSourceTypeFlag::GetPlayer() &&
            SourceTypeFlag != EFortPickupSourceTypeFlag::GetTossed() &&
            SpawnSource != EFortPickupSpawnSource::GetPlayerElimination() &&
            SpawnSource != EFortPickupSpawnSource::GetTossedByPlayer();
    }
    FFortWeaponMods::InitializePickup(
        NewPickup, Entry, bAllowRandomMods);

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
    FFortWeaponMods::FreeEntrySlots(*ItemEntry);
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

    FFortWeaponMods::InitializePickup(
        NewPickup, Entry, true);

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

void AFortInventory::TickRegeneratingItems()
{
    if (RegeneratingInventoryItems.empty() &&
        RechargingWeaponAmmo.empty())
        return;

    auto World = UWorld::GetWorld();
    if (!World)
        return;

    const double NowSeconds =
        UGameplayStatics::GetTimeSeconds(World);

    for (size_t Index = 0;
        Index < RegeneratingInventoryItems.size();)
    {
        auto& State = RegeneratingInventoryItems[Index];
        auto Owner = State.Owner.Get();
        auto AmmoDefinition = State.AmmoDefinition.Get();

        if (!Owner || !Owner->WorldInventory || !AmmoDefinition ||
            State.MaxCount <= 0 ||
            !std::isfinite(State.CooldownSeconds) ||
            State.CooldownSeconds <= 0.0)
        {
            RemoveRegenItemAt(Index);
            continue;
        }

        auto Inventory = Owner->WorldInventory;
        auto ReplicatedEntry =
            Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(
                            Candidate.ItemGuid,
                            State.ItemGuid) &&
                        Candidate.ItemDefinition == AmmoDefinition;
                },
                FFortItemEntry::Size());
        auto ItemInstance =
            Inventory->Inventory.ItemInstances.Search(
                [&](UFortWorldItem* Candidate)
                {
                    return Candidate &&
                        AreGuidsEqual(
                            Candidate->ItemEntry.ItemGuid,
                            State.ItemGuid) &&
                        Candidate->ItemEntry.ItemDefinition ==
                            AmmoDefinition;
                });

        if (!ReplicatedEntry || !ItemInstance || !*ItemInstance)
        {
            RemoveRegenItemAt(Index);
            continue;
        }

        if (ReplicatedEntry->Count >= State.MaxCount)
        {
            RemoveRegenItemAt(Index);
            continue;
        }

        if (NowSeconds < State.NextRefillTime)
        {
            ++Index;
            continue;
        }

        int32 NewCount = min(
            max(ReplicatedEntry->Count, 0) + 1,
            State.MaxCount);
        ReplicatedEntry->Count = NewCount;
        (*ItemInstance)->ItemEntry.Count = NewCount;
        (*ItemInstance)->ItemEntry.bIsDirty = true;

        const bool bReachedMaximum =
            NewCount >= State.MaxCount;
        const int32 MaximumCount = State.MaxCount;
        const auto DefinitionName =
            AmmoDefinition->Name.ToString();
        if (bReachedMaximum)
        {
            RemoveRegenItemAt(Index);
        }
        else
        {
            // One charge is restored per complete cooldown. Starting the
            // next interval from the current server time prevents a hitch
            // from granting several queued charges in a single frame.
            State.NextRefillTime =
                NowSeconds + State.CooldownSeconds;
            ++Index;
        }

        Inventory->UpdateEntry(*ReplicatedEntry);
        SDK::DbgLog(
            "[ItemRegen] refilled definition=%s count=%d max=%d\n",
            DefinitionName.c_str(),
            NewCount,
            MaximumCount);
    }

    for (size_t Index = 0;
        Index < RechargingWeaponAmmo.size();)
    {
        auto& State = RechargingWeaponAmmo[Index];
        auto Owner = State.Owner.Get();
        auto WeaponDefinition =
            State.WeaponDefinition.Get();
        if (!Owner || !Owner->WorldInventory ||
            !WeaponDefinition ||
            State.MaxLoadedAmmo <= 0 ||
            State.RechargeAmount <= 0 ||
            !std::isfinite(State.RechargeIntervalSeconds) ||
            State.RechargeIntervalSeconds <= 0.0)
        {
            RemoveRechargingWeaponAt(Index);
            continue;
        }

        auto Inventory = Owner->WorldInventory;
        auto ReplicatedEntry =
            Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(
                            Candidate.ItemGuid,
                            State.ItemGuid) &&
                        Candidate.ItemDefinition ==
                            WeaponDefinition;
                },
                FFortItemEntry::Size());
        auto ItemInstance =
            Inventory->Inventory.ItemInstances.Search(
                [&](UFortWorldItem* Candidate)
                {
                    return Candidate &&
                        AreGuidsEqual(
                            Candidate->ItemEntry.ItemGuid,
                            State.ItemGuid) &&
                        Candidate->ItemEntry.ItemDefinition ==
                            WeaponDefinition;
                });
        if (!ReplicatedEntry || !ItemInstance ||
            !*ItemInstance)
        {
            RemoveRechargingWeaponAt(Index);
            continue;
        }

        int32 CurrentLoadedAmmo =
            std::clamp(
                ReplicatedEntry->LoadedAmmo,
                0,
                State.MaxLoadedAmmo);
        if (CurrentLoadedAmmo >= State.MaxLoadedAmmo)
        {
            if (State.LastObservedLoadedAmmo !=
                State.MaxLoadedAmmo)
            {
                SyncEquippedNitroFistsAmmo(
                    Owner,
                    WeaponDefinition,
                    State.ItemGuid,
                    State.MaxLoadedAmmo);
            }
            State.LastObservedLoadedAmmo =
                State.MaxLoadedAmmo;
            State.NextRefillTime = 0.0;
            ++Index;
            continue;
        }

        // The native FN30 recharge component normally wins this race.
        // If it restored a charge, observe the increase and defer this
        // watchdog for another full interval instead of double-granting.
        if (CurrentLoadedAmmo >
            State.LastObservedLoadedAmmo)
        {
            SyncEquippedNitroFistsAmmo(
                Owner,
                WeaponDefinition,
                State.ItemGuid,
                CurrentLoadedAmmo);
            State.LastObservedLoadedAmmo =
                CurrentLoadedAmmo;
            State.NextRefillTime =
                NowSeconds +
                State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            NotifyNitroFistsRechargeStarted(
                Owner, State.ItemGuid, NowSeconds);
            ++Index;
            continue;
        }
        bool bStartedRechargeCycle = false;
        if (CurrentLoadedAmmo <
            State.LastObservedLoadedAmmo)
        {
            State.LastObservedLoadedAmmo =
                CurrentLoadedAmmo;
            if (State.NextRefillTime <= 0.0)
            {
                State.NextRefillTime =
                    NowSeconds +
                    State.RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
        }
        else if (State.NextRefillTime <= 0.0)
        {
            State.NextRefillTime =
                NowSeconds +
                State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            bStartedRechargeCycle = true;
        }
        if (bStartedRechargeCycle)
        {
            NotifyNitroFistsRechargeStarted(
                Owner, State.ItemGuid, NowSeconds);
        }

        if (NowSeconds < State.NextRefillTime)
        {
            ++Index;
            continue;
        }

        int32 NewLoadedAmmo =
            min(
                CurrentLoadedAmmo +
                    State.RechargeAmount,
                State.MaxLoadedAmmo);
        ReplicatedEntry->LoadedAmmo = NewLoadedAmmo;
        auto WorldItem = *ItemInstance;
        WorldItem->ItemEntry.LoadedAmmo =
            NewLoadedAmmo;
        WorldItem->ItemEntry.bIsDirty = true;
        State.LastObservedLoadedAmmo =
            NewLoadedAmmo;

        const int32 MaximumLoadedAmmo =
            State.MaxLoadedAmmo;
        const auto DefinitionName =
            WeaponDefinition->Name.ToString();
        if (NewLoadedAmmo >= State.MaxLoadedAmmo)
        {
            State.NextRefillTime = 0.0;
            ++Index;
        }
        else
        {
            State.NextRefillTime =
                NowSeconds +
                State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            NotifyNitroFistsRechargeStarted(
                Owner, State.ItemGuid, NowSeconds);
            ++Index;
        }

        Inventory->UpdateEntry(*ReplicatedEntry);
        BroadcastWorldItemAmmoChanged(WorldItem);
        const bool bEquippedWeaponSynced =
            SyncEquippedNitroFistsAmmo(
                Owner,
                WeaponDefinition,
                State.ItemGuid,
                NewLoadedAmmo);
        SDK::DbgLog(
            "[WeaponRecharge] fallback-refilled "
            "definition=%s ammo=%d/%d equipped-sync=%d\n",
            DefinitionName.c_str(),
            NewLoadedAmmo,
            MaximumLoadedAmmo,
            bEquippedWeaponSynced ? 1 : 0);
    }
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

using RemoveInventoryItemWithQuickBarFn = bool(*)(
    IInterface*,
    FGuid&,
    int32,
    bool,
    bool);

using RemoveInventoryItemSingleFlagFn = bool(*)(
    IInterface*,
    FGuid&,
    int32,
    bool);

enum class ERemoveInventoryItemAbi
{
    None,
    QuickBarFlagOnly,
    WithoutQuickBarFlag,
    WithQuickBarFlag
};

RemoveInventoryItemWithQuickBarFn
    RemoveInventoryItemWithQuickBarOG = nullptr;
RemoveInventoryItemSingleFlagFn
    RemoveInventoryItemQuickBarOnlyOG = nullptr;
RemoveInventoryItemSingleFlagFn
    RemoveInventoryItemWithoutQuickBarOG = nullptr;
ERemoveInventoryItemAbi RemoveInventoryItemAbi =
    ERemoveInventoryItemAbi::None;

bool CallRemoveInventoryItemOriginal(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoveFromQuickBars,
    bool bForceRemoval)
{
    if (RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithQuickBarFlag &&
        RemoveInventoryItemWithQuickBarOG)
    {
        return RemoveInventoryItemWithQuickBarOG(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    if (RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::QuickBarFlagOnly &&
        RemoveInventoryItemQuickBarOnlyOG)
    {
        return RemoveInventoryItemQuickBarOnlyOG(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars);
    }

    if (RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithoutQuickBarFlag &&
        RemoveInventoryItemWithoutQuickBarOG)
    {
        return RemoveInventoryItemWithoutQuickBarOG(
            Interface,
            ItemGuid,
            Count,
            bForceRemoval);
    }

    return false;
}

bool RemoveInventoryItemInternal(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoveFromQuickBars,
    bool bForceRemoval)
{
    if (FConfiguration::bInfiniteAmmo)
        return true;

    if (!Interface)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    auto PlayerControllerClass = FindClass("FortPlayerController");
    auto PlayerControllerSuper =
        PlayerControllerClass
            ? PlayerControllerClass->GetSuper()
            : nullptr;
    if (!PlayerControllerSuper)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    const uint64 InterfaceOffset =
        static_cast<uint64>(
            PlayerControllerSuper->GetPropertiesSize()) +
        (VersionInfo.EngineVersion >= 4.27 ? 16ull : 8ull);
    const uint64 InterfaceAddress =
        reinterpret_cast<uint64>(Interface);
    if (InterfaceAddress < InterfaceOffset)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    auto PlayerController =
        reinterpret_cast<AFortPlayerControllerAthena*>(
            InterfaceAddress - InterfaceOffset);
    if (!SDK::MemReadable(PlayerController, sizeof(void*)))
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    if (PlayerController->HasbInfiniteAmmo() &&
        PlayerController->bInfiniteAmmo)
        return true;

    auto WorldInventory = PlayerController->WorldInventory;
    if (!WorldInventory ||
        !SDK::MemReadable(WorldInventory, sizeof(void*)) ||
        Count == 0)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    auto ItemInstance =
        WorldInventory->Inventory.ItemInstances.Search(
            [&](UFortWorldItem* Item)
            {
                return Item &&
                    Item->ItemEntry.ItemGuid == ItemGuid;
            });
    auto ItemEntry =
        WorldInventory->Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Entry)
            {
                return Entry.ItemGuid == ItemGuid;
            },
            FFortItemEntry::Size());
    if (!ItemInstance || !*ItemInstance || !ItemEntry)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    auto Item = *ItemInstance;
    auto ItemDefinition = ItemEntry->ItemDefinition;
    const bool bForceRemoveItem =
        bForceRemoveFromQuickBars || bForceRemoval;

    auto AmmoDefinition =
        ItemDefinition
            ? ItemDefinition->Cast<UFortAmmoItemDefinition>()
            : nullptr;
    const float ItemLevel =
        Item->ItemEntry.Level > 0
            ? static_cast<float>(Item->ItemEntry.Level)
            : 1.0f;
    double RegenCooldownSeconds = 0.0;
    int32 RegenMaximumCount = 0;
    if (Count > 0 && !bForceRemoveItem && AmmoDefinition &&
        AmmoDefinition->HasRegenCooldown())
    {
        RegenCooldownSeconds =
            AmmoDefinition->EvaluateRegenCooldown(ItemLevel);

        // GetMaxStackSize can depend on an attribute set for some item
        // types. The pre-consumption count is an authoritative lower
        // bound and preserves the full charge capacity if that lookup is
        // unavailable in a server-only session.
        RegenMaximumCount = max(
            AmmoDefinition->GetMaxStackSize(),
            ItemEntry->Count);
        if (RegenMaximumCount <= 0 ||
            RegenMaximumCount > 10000 ||
            !std::isfinite(RegenCooldownSeconds) ||
            RegenCooldownSeconds <= 0.0 ||
            RegenCooldownSeconds > 3600.0)
        {
            RegenCooldownSeconds = 0.0;
            RegenMaximumCount = 0;
        }
    }

    bool bKeepFinalStackEmpty = false;
    if (Count > 0 &&
        !bForceRemoveItem &&
        ItemDefinition &&
        ItemDefinition->HasbPersistInInventoryWhenFinalStackEmpty() &&
        ItemDefinition->bPersistInInventoryWhenFinalStackEmpty)
    {
        auto OtherStack =
            WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Entry)
                {
                    return Entry.ItemDefinition == ItemDefinition &&
                        Entry.ItemGuid != ItemGuid &&
                        Entry.Count > 0;
                },
                FFortItemEntry::Size());
        bKeepFinalStackEmpty = !OtherStack;
    }

    const int32 RemovedCount =
        WorldInventory->RemoveItem(
            ItemGuid,
            Count,
            bKeepFinalStackEmpty);

    if (RemovedCount > 0 && RegenMaximumCount > 0)
    {
        ScheduleRegeneratingInventoryItem(
            PlayerController,
            AmmoDefinition,
            ItemGuid,
            RegenMaximumCount,
            RegenCooldownSeconds);
    }

    // Reaching a persistent empty stack or removing an already-empty stale
    // entry can legitimately remove zero units while still succeeding.
    return true;
}

bool RemoveInventoryItemWithQuickBar(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoveFromQuickBars,
    bool bForceRemoval)
{
    return RemoveInventoryItemInternal(
        Interface,
        ItemGuid,
        Count,
        bForceRemoveFromQuickBars,
        bForceRemoval);
}

bool RemoveInventoryItemWithoutQuickBar(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoval)
{
    return RemoveInventoryItemInternal(
        Interface,
        ItemGuid,
        Count,
        false,
        bForceRemoval);
}

bool RemoveInventoryItemQuickBarOnly(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoveFromQuickBars)
{
    return RemoveInventoryItemInternal(
        Interface,
        ItemGuid,
        Count,
        bForceRemoveFromQuickBars,
        false);
}

ERemoveInventoryItemAbi ResolveRemoveInventoryItemAbi()
{
    // The reflected RPC is a separate function, but its named flag changes
    // track the three interface layouts seen in the supported binaries.
    // Treat that correlation as a selector, never as permission to guess an
    // unfamiliar native ABI.
    const auto ControllerClass =
        AFortPlayerControllerAthena::StaticClass();
    const auto ControllerDefault =
        ControllerClass
            ? ControllerClass->GetDefaultObj()
            : nullptr;
    const auto ServerRemoveInventoryItem =
        ControllerDefault
            ? ControllerDefault->GetFunction(
                "ServerRemoveInventoryItem")
            : nullptr;

    if (ServerRemoveInventoryItem)
    {
        constexpr uint64 CPF_Parm =
            0x0000000000000080;
        constexpr uint64 CPF_ReturnParm =
            0x0000000000000400;
        const auto Parameters =
            ServerRemoveInventoryItem->GetParamsNamed();
        const bool bHasMetadata =
            !Parameters.NameOffsetMap.empty();
        bool bHasItemGuid = false;
        bool bHasCount = false;
        bool bHasForceRemoval = false;
        bool bHasQuickBarFlag = false;
        bool bHasUnknownParameter = false;

        for (const auto& Parameter :
            Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) ||
                (Parameter.PropertyFlags & CPF_ReturnParm))
            {
                continue;
            }

            if (Parameter.Name == "ItemGuid" ||
                Parameter.Name == "ItemGUID")
            {
                bHasItemGuid = true;
            }
            else if (Parameter.Name == "Count")
            {
                bHasCount = true;
            }
            else if (
                Parameter.Name ==
                    "bForceRemoveFromQuickBars" ||
                Parameter.Name ==
                    "bForceRemoveFromQuickbars")
            {
                bHasQuickBarFlag = true;
            }
            else if (Parameter.Name == "bForceRemoval")
            {
                bHasForceRemoval = true;
            }
            else if (Parameter.Name != "bForcePersistWhenEmpty")
            {
                bHasUnknownParameter = true;
            }
        }

        if (bHasItemGuid && bHasCount &&
            bHasForceRemoval && bHasQuickBarFlag &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::
                WithQuickBarFlag;
        }

        if (bHasItemGuid && bHasCount &&
            bHasQuickBarFlag && !bHasForceRemoval &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::
                QuickBarFlagOnly;
        }

        if (bHasItemGuid && bHasCount &&
            bHasForceRemoval && !bHasQuickBarFlag &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::
                WithoutQuickBarFlag;
        }

        // A partial or unfamiliar reflected layout is evidence that this
        // build should not receive a guessed native detour.
        if (bHasMetadata)
            return ERemoveInventoryItemAbi::None;
    }

    // These fallbacks are only for an empty reflected layout and only where
    // the native interface form is independently evidenced. Do not turn an
    // unknown or zero version into a guessed detour.
    const double FortniteVersion =
        VersionInfo.FortniteVersion;
    if (FortniteVersion == 1.72)
    {
        return ERemoveInventoryItemAbi::
            QuickBarFlagOnly;
    }

    const bool bKnownFiveArgumentBuild =
        (FortniteVersion >= 1.91 &&
            FortniteVersion <= 6.00) ||
        FortniteVersion == 1.10 ||
        FortniteVersion == 1.11 ||
        FortniteVersion == 10.40 ||
        FortniteVersion == 13.40;
    if (bKnownFiveArgumentBuild)
    {
        return ERemoveInventoryItemAbi::
            WithQuickBarFlag;
    }

    return ERemoveInventoryItemAbi::None;
}

void SetLoadedAmmo(UFortWorldItem* Item, int LoadedAmmo)
{
    if (!Item)
        return;

    auto PlayerController = (AFortPlayerControllerAthena*)Item->GetOwningController();
    if (!PlayerController || !PlayerController->WorldInventory)
        return;

    //PlayerController->WorldInventory->UpdateEntry(Item->ItemEntry);
    auto repEnt = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
        { return item.ItemGuid == Item->ItemEntry.ItemGuid; }, FFortItemEntry::Size());
    if (!repEnt)
        return;

    const int32 PreviousLoadedAmmo = repEnt->LoadedAmmo;
    repEnt->LoadedAmmo = LoadedAmmo;
    Item->ItemEntry.LoadedAmmo = LoadedAmmo;
    PlayerController->WorldInventory->UpdateEntry(*repEnt);
    Item->ItemEntry.bIsDirty = true;

    auto WeaponDefinition =
        Item->ItemEntry.ItemDefinition
            ? Item->ItemEntry.ItemDefinition->Cast<
                UFortWeaponItemDefinition>()
            : nullptr;
    if (IsNitroFistsDefinition(WeaponDefinition))
    {
        // The original native setter broadcasts this signal. The custom
        // inventory setter must preserve it so FN30's
        // FortControllerComponent_RechargeWeapons sees charge use.
        BroadcastWorldItemAmmoChanged(Item);
        ObserveRechargingWeaponAmmo(
            PlayerController,
            WeaponDefinition,
            Item->ItemEntry.ItemGuid,
            Item->ItemEntry.Level,
            PreviousLoadedAmmo,
            LoadedAmmo);
    }
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

    const auto RemoveInventoryItemAddress =
        FindRemoveInventoryItem();
    RemoveInventoryItemAbi =
        ResolveRemoveInventoryItemAbi();

    bool bRemoveInventoryItemHooked = false;
    if (RemoveInventoryItemAddress &&
        RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithQuickBarFlag)
    {
        Utils::Hook(
            RemoveInventoryItemAddress,
            RemoveInventoryItemWithQuickBar,
            RemoveInventoryItemWithQuickBarOG);
        bRemoveInventoryItemHooked =
            RemoveInventoryItemWithQuickBarOG != nullptr;
    }
    else if (RemoveInventoryItemAddress &&
        RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::QuickBarFlagOnly)
    {
        Utils::Hook(
            RemoveInventoryItemAddress,
            RemoveInventoryItemQuickBarOnly,
            RemoveInventoryItemQuickBarOnlyOG);
        bRemoveInventoryItemHooked =
            RemoveInventoryItemQuickBarOnlyOG != nullptr;
    }
    else if (RemoveInventoryItemAddress &&
        RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithoutQuickBarFlag)
    {
        Utils::Hook(
            RemoveInventoryItemAddress,
            RemoveInventoryItemWithoutQuickBar,
            RemoveInventoryItemWithoutQuickBarOG);
        bRemoveInventoryItemHooked =
            RemoveInventoryItemWithoutQuickBarOG != nullptr;
    }

    const char* RemoveInventoryItemAbiName = "unresolved";
    switch (RemoveInventoryItemAbi)
    {
    case ERemoveInventoryItemAbi::QuickBarFlagOnly:
        RemoveInventoryItemAbiName = "quickbar-only";
        break;
    case ERemoveInventoryItemAbi::WithoutQuickBarFlag:
        RemoveInventoryItemAbiName = "force-removal-only";
        break;
    case ERemoveInventoryItemAbi::WithQuickBarFlag:
        RemoveInventoryItemAbiName = "quickbar-and-force-removal";
        break;
    default:
        break;
    }
    SDK::DbgLog(
        "  [FI] 2 RemoveInventoryItem %s "
        "(abi=%s target=%p)\n",
        bRemoveInventoryItemHooked
            ? "hooked"
            : "unavailable",
        RemoveInventoryItemAbiName,
        reinterpret_cast<void*>(
            RemoveInventoryItemAddress));
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
            const uint32 LoadedAmmoSetterIndex =
                uint32(
                    SetOwningInventoryIdx -
                    (HasPhantomReserveAmmo
                        ? (VersionInfo.EngineVersion < 4.27
                            ? 2
                            : 3)
                        : 1));

            Utils::Hook<UFortWorldItem>(
                LoadedAmmoSetterIndex,
                SetLoadedAmmo);
            if (HasPhantomReserveAmmo)
                Utils::Hook<UFortWorldItem>(uint32(SetOwningInventoryIdx - (VersionInfo.EngineVersion < 4.27 ? 1 : 2)), SetPhantomReserveAmmo);

            SDK::DbgLog(
                "[WeaponRecharge] loaded-ammo setter hook "
                "installed index=%u\n",
                LoadedAmmoSetterIndex);
        }
        else
        {
            SDK::DbgLog(
                "[WeaponRecharge] loaded-ammo setter hook "
                "unavailable: owner setter vft index missing\n");
        }
    }
    else
    {
        SDK::DbgLog(
            "[WeaponRecharge] loaded-ammo setter hook "
            "unavailable: owner setter signature missing\n");
    }

    SDK::DbgLog("  [FI] 3 SetOwningInventory block done\n");
    if (auto sd = DefaultObjImpl("FortAthenaSupplyDrop"))
        Utils::ExecHook(sd->GetFunction("SpawnPickup"), SpawnPickup_);
    SDK::DbgLog("  [FI] 4 PostLoadHook complete\n");
}
