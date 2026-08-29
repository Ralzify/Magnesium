#pragma once
#include "../../pch.h"

class UFortWeaponItemDefinition;
struct FFortItemEntry;
class AFortPickupAthena;
class AFortWeapon;
class AFortPlayerControllerAthena;

class FFortWeaponMods
{
public:
    static bool IsSupported();
    static bool HasEntrySlots(const FFortItemEntry& Entry);

    static bool CopyDefinitionSlotsToEntry(const UFortWeaponItemDefinition* WeaponDefinition,
        FFortItemEntry& Destination);
    static bool CopyEntrySlots(const FFortItemEntry& Source, FFortItemEntry& Destination);
    static void FreeEntrySlots(FFortItemEntry& Entry);

    static void InitializePickup(AFortPickupAthena* Pickup, const FFortItemEntry& SourceEntry,
        bool bAllowRandomMods);
    static bool ApplyEntrySlotsAfterEquip(AFortWeapon* Weapon, const FFortItemEntry& Entry);
    static bool SyncWeaponSlotsToInventory(AFortPlayerControllerAthena* PlayerController,
        AFortWeapon* Weapon);
    static void EnsureBenchHooks();

private:
    DefUHookOg(ServerPurchaseWeaponModForWeapon_);
    DefUHookOg(ServerPurchaseRemoveMod_);

    InitPostLoadHooks;
};
