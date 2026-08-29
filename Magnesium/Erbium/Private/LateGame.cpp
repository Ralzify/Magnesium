#include "pch.h"
#include "../Public/LateGame.h"
#include "../Public/Utils.h"
#include "../../FortniteGame/Public/FortInventory.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../FortniteGame/Public/FortControllerComponent_VictoryCrowns.h"
#include "../Magnesium/Erbium/Public/Misc.h"
#include "../Public/Configuration.h"

#include <random>
#include <chrono>
#include <vector>

namespace
{
    bool VersionIn(double MinInclusive, double MaxExclusive)
    {
        return VersionInfo.FortniteVersion >= MinInclusive &&
            VersionInfo.FortniteVersion < MaxExclusive;
    }

    bool VersionIs(double Exact)
    {
        return VersionInfo.FortniteVersion == Exact;
    }

    bool SeasonIs(int Season)
    {
        return std::floor(VersionInfo.FortniteVersion) == Season;
    }
}

static bool IsUsableLateGameObject(const UObject* Object)
{
    if (!Object || IsBadReadPtr((void*)Object))
        return false;

    if (Object->Index < 0 || Object->Index >= TUObjectArray::Num())
    {
        return false;
    }

    auto Item = TUObjectArray::GetItemByIndex(Object->Index);
    return Item && Item->Object == Object && !(Item->Flags & 0x20) && Object->Class &&
        !IsBadReadPtr(Object->Class);
}

static bool IsPersistentLateGameInventoryItem(const UFortItemDefinition* ItemDefinition)
{
    if (!ItemDefinition || !ItemDefinition->HasItemType())
        return false;

    const auto ItemType = ItemDefinition->ItemType;
    return ItemType == EFortItemType::GetWeaponHarvest() ||
        ItemType == EFortItemType::GetBuildingPiece() || ItemType == EFortItemType::GetEditTool();
}

static int32 ClearInventoryForLateGameLoadout(AFortInventory* WorldInventory)
{
    if (!WorldInventory)
        return 0;

    std::vector<FGuid> GuidsToRemove;
    auto& Entries = WorldInventory->Inventory.ReplicatedEntries;
    GuidsToRemove.reserve(Entries.Num());

    for (int32 Index = 0; Index < Entries.Num(); ++Index)
    {
        auto& Entry = Entries.Get(Index, FFortItemEntry::Size());
        if (Entry.ItemDefinition && IsPersistentLateGameInventoryItem(Entry.ItemDefinition))
        {
            continue;
        }

        GuidsToRemove.push_back(Entry.ItemGuid);
    }

    for (const auto& Guid : GuidsToRemove)
        WorldInventory->Remove(Guid);

    return static_cast<int32>(GuidsToRemove.size());
}

static bool HasResolvableLateGamePrimaryItem(const TArray<TArray<TPair<FString, int>>>& Slots)
{
    const int32 PrimarySlotCount = Slots.Num() < 5 ? Slots.Num() : 5;
    for (int32 SlotIndex = 0;
        SlotIndex < PrimarySlotCount;
        ++SlotIndex)
    {
        const auto& Slot = Slots[SlotIndex];
        for (int32 CandidateIndex = 0;
            CandidateIndex < Slot.Num();
            ++CandidateIndex)
        {
            if (FindObject<UFortWorldItemDefinition>(Slot[CandidateIndex].Key().CStr()))
            {
                return true;
            }
        }
    }

    return false;
}

static UFortControllerComponent_VictoryCrowns* GetLateGameVictoryCrownComponent(
    AFortPlayerControllerAthena* PlayerController)
{
    if (!IsUsableLateGameObject(PlayerController))
        return nullptr;

    auto CrownComponentClass = UFortControllerComponent_VictoryCrowns::StaticClass();
    if (!IsUsableLateGameObject(CrownComponentClass))
    {
        CrownComponentClass = FindClass("FortControllerComponent_VictoryCrowns");
    }
    auto RawCrownComponent = IsUsableLateGameObject(CrownComponentClass)
            ? PlayerController->GetComponentByClass(CrownComponentClass) : nullptr;
    return IsUsableLateGameObject(RawCrownComponent) && RawCrownComponent->IsA(CrownComponentClass)
            ? (UFortControllerComponent_VictoryCrowns*)
                RawCrownComponent : nullptr;
}

static const UFortWorldItemDefinition* ResolveLateGameVictoryCrownDefinition(
    UFortControllerComponent_VictoryCrowns* CrownComponent)
{
    auto FallbackDefinition = FindObject<UFortWorldItemDefinition>(
            L"/VictoryCrownsGameplay/Items/AGID_VictoryCrown.AGID_VictoryCrown");
    if (IsUsableLateGameObject(FallbackDefinition))
        return FallbackDefinition;

    if (CrownComponent && CrownComponent->HasCrownInventoryItemClass())
    {
        auto& CrownInventoryItemClass = CrownComponent->GetCrownInventoryItemClass();
        auto ConfiguredDefinition = CrownInventoryItemClass.Get();
        if (IsUsableLateGameObject(ConfiguredDefinition))
            return ConfiguredDefinition;
    }

    return nullptr;
}

TArray<TArray<TPair<FString, int>>> LateGame::GetLoadout()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    TArray<TArray<TPair<FString, int>>> Slots;

    // Slot 1 (Assault Rifles)
    TArray<TPair<FString, int>> Slot1;

    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_R_Ore_T03.WID_Assault_Auto_Athena_R_Ore_T03"), 1));
    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_VR_Ore_T03.WID_Assault_AutoHigh_Athena_VR_Ore_T03"), 1));
    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_SR_Ore_T03.WID_Assault_AutoHigh_Athena_SR_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 2.50)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_VR_Ore_T03.WID_Assault_LMG_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_SR_Ore_T03.WID_Assault_LMG_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 5.40)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Suppressed_Athena_VR_Ore_T03.WID_Assault_Suppressed_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Suppressed_Athena_SR_Ore_T03.WID_Assault_Suppressed_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.01)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_AR.WID_Boss_Adventure_AR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Hos_MG.WID_Boss_Hos_MG"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Midas.WID_Boss_Midas"), 1));
    }

    if (VersionIn(12.30, 13.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Donut.WID_Pistol_Donut"), 1));
    }

    if (SeasonIs(14))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_R_Ore_T03.WID_Assault_Stark_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_VR_Ore_T03.WID_Assault_Stark_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_SR_Ore_T03.WID_Assault_Stark_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.20)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_HopRockDualies.WID_WaffleTruck_HopRockDualies"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_R.WID_Assault_RedDotAR_Athena_R"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_VR.WID_Assault_RedDotAR_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_SR.WID_Assault_RedDotAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_SR.WID_Assault_CoreAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_VR.WID_Assault_CoreAR_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 20.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/RedDotBurst/WID_Assault_RedDotBurstAR_Athena_SR.WID_Assault_RedDotBurstAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/RedDotBurst/WID_Assault_RedDotBurstAR_Athena_VR.WID_Assault_RedDotBurstAR_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Assault/WID_Assault_Heavy_Recoil_Athena_SR.WID_Assault_Heavy_Recoil_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Assault/WID_Assault_Heavy_Recoil_Athena_VR.WID_Assault_Heavy_Recoil_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 23.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_SR.WID_Assault_MusterScoped_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_VR.WID_Assault_MusterScoped_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalCoreAR/WID_Assault_Radical_CoreAR_Athena_SR.WID_Assault_Radical_CoreAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalCoreAR/WID_Assault_Radical_CoreAR_Athena_VR.WID_Assault_Radical_CoreAR_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/PulseRifleMMObj/WID_Assault_PastaRipper_Athena_MMObj.WID_Assault_PastaRipper_Athena_MMObj"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_SR.WID_Assault_Chrono_Pan_Rifle_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_VR.WID_Assault_Chrono_Pan_Rifle_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 26.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_SR.WID_Assault_FlipMag_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_VR.WID_Assault_FlipMag_Athena_VR"), 1));
    }

    // will add ch5 ars once the damage is fixed

    Slots.Add(Slot1);

    TArray<TPair<FString, int>> Slot2;
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_C_Ore_T03.WID_Shotgun_Standard_Athena_C_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_UC_Ore_T03.WID_Shotgun_Standard_Athena_UC_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion <= 8.51)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_R_Ore_T03.WID_Shotgun_SemiAuto_Athena_R_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_VR_Ore_T03.WID_Shotgun_SemiAuto_Athena_VR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 3.31)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_VR.WID_Shotgun_SlugFire_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_SR.WID_Shotgun_SlugFire_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 6.31)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_VR_Ore_T03.WID_Shotgun_Standard_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_SR_Ore_T03.WID_Shotgun_Standard_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_R_Ore_T03.WID_Shotgun_Combat_Athena_R_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_VR_Ore_T03.WID_Shotgun_Combat_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_SR_Ore_T03.WID_Shotgun_Combat_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.40)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_Dub.WID_WaffleTruck_Dub"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_UC.WID_Shotgun_CoreBurst_Athena_UC"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_R.WID_Shotgun_CoreBurst_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_VR.WID_Shotgun_CoreBurst_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_SR.WID_Shotgun_CoreBurst_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 20.20)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_SR_Ore_T03.WID_Shotgun_Break_Action_Athena_SR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_VR_Ore_T03.WID_Shotgun_Break_Action_Athena_VR_Ore_T03"), 1));
    }
    if (VersionInfo.FortniteVersion >= 21.30)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_SR.WID_Shotgun_OverLoad_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_VR.WID_Shotgun_OverLoad_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 23.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_SR.WID_Shotgun_MusterPump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_VR.WID_Shotgun_MusterPump_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_UR.WID_Shotgun_RadicalPump_Athena_UR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_SR.WID_Shotgun_RadicalPump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_VR.WID_Shotgun_RadicalPump_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.11)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_SR.WID_Shotgun_Chrono_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_VR.WID_Shotgun_Chrono_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 28.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_UR_Boss.WID_Shotgun_Pump_Paprika_Athena_UR_Boss"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_SR.WID_Shotgun_Pump_Paprika_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_VR.WID_Shotgun_Pump_Paprika_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_UR_Boss.WID_Shotgun_Auto_Paprika_Athena_UR_Boss"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_SR.WID_Shotgun_Auto_Paprika_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_VR.WID_Shotgun_Auto_Paprika_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 29.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_UR.WID_Shotgun_Break_Cerberus_Athena_UR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_SR.WID_Shotgun_Break_Cerberus_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_VR.WID_Shotgun_Break_Cerberus_Athena_VR"), 1));
    }

    Slots.Add(Slot2);

    // Slot 3 (Snipers)
    TArray<TPair<FString, int>> Slot3;
    Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03"), 1));
    Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03"), 1));
    Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 3.10)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_UC_Ore_T03.WID_Sniper_NoScope_Athena_UC_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_R_Ore_T03.WID_Sniper_NoScope_Athena_R_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 5.21)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_VR_Ore_T03.WID_Sniper_Heavy_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_SR_Ore_T03.WID_Sniper_Heavy_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 7.10)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 13.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.10)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_UC.WID_Sniper_Cowboy_Athena_UC"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_R.WID_Sniper_Cowboy_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_VR.WID_Sniper_Cowboy_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 17.40)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_UC.WID_Sniper_CoreSniper_Athena_UC"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_R.WID_Sniper_CoreSniper_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_VR.WID_Sniper_CoreSniper_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_SR.WID_Sniper_CoreSniper_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 20.10)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_R_Ore_T03.WID_Sniper_Heavy_Athena_R_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_UR_Ore_T03.WID_Sniper_Heavy_Athena_UR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.11)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ExplosiveRepeater/WID_Sniper_ExplosiveRepeater_Athena_SR.WID_Sniper_ExplosiveRepeater_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ExplosiveRepeater/WID_Sniper_ExplosiveRepeater_Athena_VR.WID_Sniper_ExplosiveRepeater_Athena_VR"), 1));
    }

    Slots.Add(Slot3);

    // Slot 4 (Heals)
    TArray<TPair<FString, int>> Slot4;
    Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Medkit/Athena_Medkit.Athena_Medkit"), 3));
    Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Shields/Athena_Shields.Athena_Shields"), 3));

    if (VersionInfo.FortniteVersion >= 1.80)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff"), 2));
    }

    if (VersionInfo.FortniteVersion >= 1.11)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShieldSmall/Athena_ShieldSmall.Athena_ShieldSmall"), 6));
    }

    if (VersionInfo.FortniteVersion >= 2.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SuperMedkit/Athena_SuperMedkit.Athena_SuperMedkit"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ChillBronco/Athena_ChillBronco.Athena_ChillBronco"), 6));
    }

    if (VersionInfo.FortniteVersion >= 11.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/WID_Athena_Flopper.WID_Athena_Flopper"), 4));
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/Effective/WID_Athena_Flopper_Effective.WID_Athena_Flopper_Effective"), 3));
    }

    if (VersionInfo.FortniteVersion >= 13.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/BottomlessChugJug/WID_Athena_BottomlessChugJug.WID_Athena_BottomlessChugJug"), 1));
    }

    if (VersionInfo.FortniteVersion >= 14.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/HopFlopper/WID_Athena_Flopper_HopFlopper.WID_Athena_Flopper_HopFlopper"), 3));
    }

    if (VersionInfo.FortniteVersion >= 15.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/RiftFlopper/WID_Athena_Flopper_Rift.WID_Athena_Flopper_Rift"), 2));
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/ZeroFlopper/WID_Athena_Flopper_Zero.WID_Athena_Flopper_Zero"), 3));
    }

    if (VersionInfo.FortniteVersion >= 23.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/MusterConsumables/Items/EnergyDrink/Items/WID_Athena_EnergyDrink.WID_Athena_EnergyDrink"), 6));
    }

    Slots.Add(Slot4);

    // Slot 5 (Consumables)
    TArray<TPair<FString, int>> Slot5;

    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_R_Ore_T03.WID_Launcher_Rocket_Athena_R_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_VR_Ore_T03.WID_Launcher_Rocket_Athena_VR_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_SR_Ore_T03.WID_Launcher_Rocket_Athena_SR_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_R_Ore_T03.WID_Launcher_Grenade_Athena_R_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_VR_Ore_T03.WID_Launcher_Grenade_Athena_VR_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_SR_Ore_T03.WID_Launcher_Grenade_Athena_SR_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/KnockGrenade/Athena_KnockGrenade.Athena_KnockGrenade"), 9));

    if (VersionInfo.FortniteVersion >= 3.4 && VersionInfo.FortniteVersion <= 7.10)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_VR_T03.WID_RC_Rocket_Athena_VR_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_SR_T03.WID_RC_Rocket_Athena_SR_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 3.5)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/TowerGrenade/Athena_TowerGrenade.Athena_TowerGrenade"), 2));
    }

    if (VersionInfo.FortniteVersion >= 5.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item"), 2));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 5.40)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Hook_Gun_VR_Ore_T03.WID_Hook_Gun_VR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 5.41)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SuperTowerGrenade/Levels/PortAFort_A/Athena_SuperTowerGrenade_A.Athena_SuperTowerGrenade_A"), 1));
    }

    if (VersionInfo.FortniteVersion >= 6.02)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_VR_Ore_T03.WID_Launcher_Military_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_SR_Ore_T03.WID_Launcher_Military_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 7.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 8.11)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_C.WID_Pistol_Flintlock_Athena_C"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_UC.WID_Pistol_Flintlock_Athena_UC"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/DeployableStorm/Athena_DogSweater.Athena_DogSweater"), 1));
    }

    if (VersionInfo.FortniteVersion >= 10.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2"), 2));
    }

    if (VersionInfo.FortniteVersion >= 11.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Bucket/WID_Athena_Bucket_Old.WID_Athena_Bucket_Old"), 4));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun"), 6));
    }

    if (VersionInfo.FortniteVersion >= 13.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_GrapplingHoot.WID_Boss_GrapplingHoot"), 1));
    }

    if (SeasonIs(14))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Repulsors/CoreBR/WID_HighTower_Tomato_Repulsor_CoreBR.WID_HighTower_Tomato_Repulsor_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/RepulsorCannon/CoreBR/WID_HighTower_Tomato_RepulsorCannon_CoreBR.WID_HighTower_Tomato_RepulsorCannon_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Date/ChainLightning/CoreBR/WID_HighTower_Date_ChainLightning_CoreBR.WID_HighTower_Date_ChainLightning_CoreBR"), 1));
    }

    if (VersionIs(15.50) || VersionIs(17.30))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/LTM/Builder/Gameplay/Yeetknock_pistol/Builder_WID_YEETknock_UR.Builder_WID_YEETknock_UR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 17.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/WID_Athena_Mother_Scooter.WID_Athena_Mother_Scooter"), 1));
    }

    if (VersionInfo.FortniteVersion >= 18.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Consumables/IcyGrapple/WID_Athena_IcyGrapple.WID_Athena_IcyGrapple"), 1));
    }

    if (SeasonIs(19))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ParallelGameplay/Items/WestSausage/WID_WestSausage_Parallel.WID_WestSausage_Parallel"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.01)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/ShieldGenerator/WID_Athena_ShieldGenerator.WID_Athena_ShieldGenerator"), 2));
    }

    if (VersionInfo.FortniteVersion >= 21.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/GrappleGloves/Items/GrappleGloves/WID_GrappleGloves.WID_GrappleGloves"), 1));
    }

    if (VersionInfo.FortniteVersion >= 13.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Prototype/WID_FringePlank_Athena_Prototype.WID_FringePlank_Athena_Prototype"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.10)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Fringeplank_Firework/Items/WIDs/WID_Firework_Gun.WID_Firework_Gun"), 1));
    }

    if (VersionInfo.FortniteVersion >= 22.10)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/LaunchPadItemGameplay/Items/WID/WID_Athena_LaunchPadThrown.WID_Athena_LaunchPadThrown"), 2));
    }

    if (VersionInfo.FortniteVersion >= 23.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ShockwaveMace/Items/WID_Muster_ShockwaveMace.WID_Muster_ShockwaveMace"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ShockwaveMace/Items/WID_Muster_ShockwaveMace_Mythic.WID_Muster_ShockwaveMace_Mythic"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/KatanaGameplay/Items/Katana/WID_Melee_Katana.WID_Melee_Katana"), 1));
    }

    if (VersionIs(24.20))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/DryBox/Items/NyxGlass/AGID_NyxGlass.AGID_NyxGlass"), 1));
    }

    if (VersionInfo.FortniteVersion >= 26.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/Proto/WID_Athena_AppleSunSmall.WID_Athena_AppleSunSmall"), 6));
        Slot5.Add(TPair<FString, int>(TEXT("/RocketRamGameplay/Items/RocketRam/WID_RocketRam.WID_RocketRam"), 1));
    }

    if (VersionInfo.FortniteVersion >= 29.01)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ShieldBubble/Gameplay/ShieldBubbleJr/Athena_SilverBlazer_Mini_UC.Athena_SilverBlazer_Mini_UC"), 4));
    }

    Slots.Add(Slot5);

    // Slot 6 (Traps)
    TArray<TPair<FString, int>> Traps;
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Floor_Player_Launch_Pad_Athena.TID_Floor_Player_Launch_Pad_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_BouncePad_Athena.TID_Context_BouncePad_Athena"), 3));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_ContextTrap_Athena.TID_ContextTrap_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_Reinforced_Athena.TID_Context_Reinforced_Athena"), 2));

    Slots.Add(Traps);

    // Slot 7 (Materials)
    std::uniform_int_distribution<int> Mats(186, 646);
    std::uniform_int_distribution<int> Gold(1200, 7500);

    TArray<TPair<FString, int>> Materials;
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/WoodItemData.WoodItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/StoneItemData.StoneItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/MetalItemData.MetalItemData"), Mats(rng)));

    if (VersionInfo.FortniteVersion >= 15)
    {
        Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/Athena_WadsItemData.Athena_WadsItemData"), Gold(rng)));
    }

    Slots.Add(Materials);

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 186);
    std::uniform_int_distribution<int> Shells(87, 576);
    std::uniform_int_distribution<int> Medium(124, 824);
    std::uniform_int_distribution<int> Light(186, 824);
    std::uniform_int_distribution<int> Rockets(3, 12);
    std::uniform_int_distribution<int> STW(186, 999);
    std::uniform_int_distribution<int> Arrows(12, 30);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells"), Shells(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium"), Medium(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight"), Light(rng)));

    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AmmoDataRockets.AmmoDataRockets"), 12));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataExplosive.AmmoDataExplosive"), STW(rng))); // make these and below optional???
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataEnergyCell.AmmoDataEnergyCell"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsHeavy.AmmoDataBulletsHeavy"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataShells.AmmoDataShells"), STW(rng)));

    if (VersionInfo.FortniteVersion >= 16.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Ammo/AthenaAmmoDataArrows.AthenaAmmoDataArrows"), Arrows(rng)));
    }

    Slots.Add(Ammo);

    return Slots;
}

TArray<TArray<TPair<FString, int>>> LateGame::GetVersionizedLoadout()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    static std::mt19937 Randomization(std::random_device{}());
    std::uniform_int_distribution<int> Distribution(1, 5);

    TArray<TArray<TPair<FString, int>>> Slots;

    // The cert table stores 1.10 as 1.1, so ordinary checks like >= 1.6 reject both it and 1.11.
    const bool bSeasonOneDoubleDigitPatch = VersionIs(1.10) || VersionIs(1.11);

    // Slot 1 (Assault Rifles)
    TArray<TPair<FString, int>> Slot1;

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 19.00) || SeasonIs(27))
    {
        int Roll = Distribution(Randomization);

        if (Roll == 1)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_UC_Ore_T03.WID_Assault_Auto_Athena_UC_Ore_T03"), 1));
        }

        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_R_Ore_T03.WID_Assault_Auto_Athena_R_Ore_T03"), 1));
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 19.00) || SeasonIs(23) || SeasonIs(27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_VR_Ore_T03.WID_Assault_AutoHigh_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_SR_Ore_T03.WID_Assault_AutoHigh_Athena_SR_Ore_T03"), 1));
    }

    if (SeasonIs(23))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_UC_Ore_T03.WID_Assault_AutoHigh_Athena_UC_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_R_Ore_T03.WID_Assault_AutoHigh_Athena_R_Ore_T03"), 1));
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 7.10) || VersionIs(9.30) ||
        VersionIn(11.00, 15.00) || VersionIn(17.00, 19.00) || VersionIn(23.10, 24.00))
    {
        int Roll = Distribution(Randomization);

        if (Roll == 1)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_UC_Ore_T03.WID_Assault_SemiAuto_Athena_UC_Ore_T03"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_R_Ore_T03.WID_Assault_SemiAuto_Athena_R_Ore_T03"), 1));
        }
    }

    if (VersionIn(4.2, 7.30) || VersionIs(9.30) || VersionIn(11.00, 15.00) ||
        VersionIn(17.00, 19.00) || VersionIn(23.10, 24.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_VR_Ore_T03.WID_Assault_SemiAuto_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_SR_Ore_T03.WID_Assault_SemiAuto_Athena_SR_Ore_T03"), 1));
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.63, 11.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_R_Ore_T03.WID_Assault_Surgical_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_VR_Ore_T03.WID_Assault_Surgical_Athena_VR_Ore_T03"), 1));
    }

    if (VersionIn(14.00, 14.20))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_UC_Ore_T03.WID_Assault_Surgical_Athena_UC_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_R_Ore_T03.WID_Assault_Surgical_Athena_R_Ore_T03"), 1));
    }

    if (SeasonIs(14) || SeasonIs(27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_VR_Ore_T03.WID_Assault_Surgical_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_SR_Ore_T03.WID_Assault_Surgical_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(2.50, 11.00) || SeasonIs(12) || (VersionIs(27.10) || VersionIs(27.11)))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_VR_Ore_T03.WID_Assault_LMG_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_SR_Ore_T03.WID_Assault_LMG_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(3.50, 6.00) || VersionIs(9.30) || SeasonIs(14) || VersionIs(27.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_R_Ore_T03.WID_Assault_LMGSAW_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_VR_Ore_T03.WID_Assault_LMGSAW_Athena_VR_Ore_T03"), 1));
    }

    if (VersionIn(20.20, 21.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_R_Ore_T03.WID_Assault_LMGSAW_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_VR_Ore_T03.WID_Assault_LMGSAW_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_SR_Ore_T03.WID_Assault_LMGSAW_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(4.40, 7.20) || VersionIn(7.30, 9.00) || VersionIs(9.30) || SeasonIs(20))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Thermal_Athena_VR_Ore_T03.WID_Assault_Surgical_Thermal_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Thermal_Athena_SR_Ore_T03.WID_Assault_Surgical_Thermal_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(4.50, 5.40) || VersionIn(8.51, 10.20) || VersionIs(11.31) || VersionIs(27.11))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoDrum_Athena_UC_Ore_T03.WID_Assault_AutoDrum_Athena_UC_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoDrum_Athena_R_Ore_T03.WID_Assault_AutoDrum_Athena_R_Ore_T03"), 1));
    }

    if (VersionIn(5.40, 9.00) || VersionIn(10.20, 11.00) || VersionIs(11.3) || SeasonIs(12) ||
        VersionIs(17.40) || SeasonIs(18) || SeasonIs(27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Suppressed_Athena_VR_Ore_T03.WID_Assault_Suppressed_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Suppressed_Athena_SR_Ore_T03.WID_Assault_Suppressed_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(6.22, 11.00) || VersionIn(11.40, 13.00) || SeasonIs(15) || SeasonIs(17) || SeasonIs(27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Heavy_Athena_R_Ore_T03.WID_Assault_Heavy_Athena_R_Ore_T03"), 1));
    }

    if (VersionIn(6.22, 8.10) || VersionIn(11.40, 13.00) || SeasonIs(15) || SeasonIs(17) || SeasonIs(27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Heavy_Athena_VR_Ore_T03.WID_Assault_Heavy_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Heavy_Athena_SR_Ore_T03.WID_Assault_Heavy_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(7.40, 11.00) || VersionIs(11.31) || VersionIn(16.30, 17.00) || VersionIs(23.50) ||
        VersionIs(27.11))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_R.WID_Assault_Infantry_Athena_R"), 1));
    }

    if (VersionIn(8.40, 11.00) || VersionIs(11.3) || VersionIn(16.30, 17.00) || VersionIs(23.50) ||
        VersionIs(27.11))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_VR.WID_Assault_Infantry_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_SR.WID_Assault_Infantry_Athena_SR"), 1));
    }

    if (VersionIn(9.01, 10.00) || VersionIs(11.3) || SeasonIs(15) || SeasonIs(23))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03"), 1));
    }

    // todo: add first order blaster rifle

    if (SeasonIs(12))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_AR.WID_Boss_Adventure_AR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Hos_MG.WID_Boss_Hos_MG"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Midas.WID_Boss_Midas"), 1));
    }

    if (VersionIn(12.30, 13.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Donut.WID_Pistol_Donut"), 1));
    }

    if (SeasonIs(13))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Midas.WID_Boss_Midas"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_UR_Ore_T03.WID_Assault_SemiAuto_Athena_UR_Ore_T03"), 1));
    }

    if (SeasonIs(14))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_R_Ore_T03.WID_Assault_Stark_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_VR_Ore_T03.WID_Assault_Stark_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_SR_Ore_T03.WID_Assault_Stark_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIs(14.40))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_GhostMidas.WID_Boss_GhostMidas"), 1));
    }

    if (VersionIs(11.3) || SeasonIs(15) || VersionIs(20.30) || VersionIn(21.10, 22.00) ||
        VersionIs(22.30) || VersionIs(29.40))
    {
        if (VersionIs(29.40))
        {
            Slot1.Add(TPair<FString, int>(TEXT("/Galileo/Items/Blaster/WID_Cosmos_AR_Athena_UC.WID_Cosmos_AR_Athena_UC"), 1));
        }
        else if (SeasonIs(15))
        {
            Slot1.Add(TPair<FString, int>(TEXT("/CosmosGameplay/Items/CosmoAssaultRifle/WID_Cosmos_AR_Athena_UC.WID_Cosmos_AR_Athena_UC"), 1));
        }
        else
        {
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Prototype/Galileo_Bun/WID_Galileo_Bun_Athena_UC.WID_Galileo_Bun_Athena_UC"), 1));
        }
    }

    if (VersionIn(15.20, 17.00) || VersionIs(19.00) || VersionIs(23.40))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_HopRockDualies.WID_WaffleTruck_HopRockDualies"), 1));
    }

    if (SeasonIs(17))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_Athena_R_Ore_T03.WID_Assault_PastaRipper_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_Athena_VR_Ore_T03.WID_Assault_PastaRipper_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_Athena_SR_Ore_T03.WID_Assault_PastaRipper_Athena_SR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_Athena_Boss.WID_Assault_PastaRipper_Athena_Boss"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/LlamaRoaster/WID_Assault_LlamaRoaster_Boss.WID_Assault_LlamaRoaster_Boss"), 1));
    }

    if (VersionIn(17.40, 18.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_E.WID_Assault_PastaRipper_E"), 1));
    }

    if (SeasonIs(18) || VersionIs(20.40))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/PowerUp/LMG/WID_Assault_LMG_Powerup_Athena_VR.WID_Assault_LMG_Powerup_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/PowerUp/LMG/WID_Assault_LMG_Powerup_Athena_SR.WID_Assault_LMG_Powerup_Athena_SR"), 1));
    }

    if (VersionIn(18.20, 19.00) || VersionIs(20.00) || VersionIn(21.20, 22.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/ar/WID_Assault_Recoil_Athena_R.WID_Assault_Recoil_Athena_R"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/ar/WID_Assault_Recoil_Athena_VR.WID_Assault_Recoil_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/ar/WID_Assault_Recoil_Athena_SR.WID_Assault_Recoil_Athena_SR"), 1));
    }

    if (VersionIn(19, 23.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_SR.WID_Assault_CoreAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_VR.WID_Assault_CoreAR_Athena_VR"), 1));
    }

    if (SeasonIs(19) || VersionIn(20.00, 21.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_R.WID_Assault_RedDotAR_Athena_R"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_VR.WID_Assault_RedDotAR_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_SR.WID_Assault_RedDotAR_Athena_SR"), 1));
    }

    if (VersionIn(20.00, 22.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/RedDotBurst/WID_Assault_RedDotBurstAR_Athena_SR.WID_Assault_RedDotBurstAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/RedDotBurst/WID_Assault_RedDotBurstAR_Athena_VR.WID_Assault_RedDotBurstAR_Athena_VR"), 1));
    }

    if (SeasonIs(22))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Assault/WMID_Assault_Chrome_Athena_R.WMID_Assault_Chrome_Athena_R"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Assault/WMID_Assault_Chrome_Athena_VR.WMID_Assault_Chrome_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Assault/WMID_Assault_Chrome_Athena_SR.WMID_Assault_Chrome_Athena_SR"), 1));
    }

    if (VersionIn(21.00, 23.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Assault/WID_Assault_Heavy_Recoil_Athena_SR.WID_Assault_Heavy_Recoil_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Assault/WID_Assault_Heavy_Recoil_Athena_VR.WID_Assault_Heavy_Recoil_Athena_VR"), 1));
    }

    if (VersionIn(23.00, 25.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_SR.WID_Assault_MusterScoped_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_VR.WID_Assault_MusterScoped_Athena_VR"), 1));
    }

    if (SeasonIs(24))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/PulseRifleMMObj/WID_Assault_PastaRipper_Athena_MMObj.WID_Assault_PastaRipper_Athena_MMObj"), 1));
    }

    if (VersionIn(24.00, 27.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalCoreAR/WID_Assault_Radical_CoreAR_Athena_SR.WID_Assault_Radical_CoreAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalCoreAR/WID_Assault_Radical_CoreAR_Athena_VR.WID_Assault_Radical_CoreAR_Athena_VR"), 1));
    }

    if (SeasonIs(25))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_SR.WID_Assault_Chrono_Pan_Rifle_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_VR.WID_Assault_Chrono_Pan_Rifle_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotARS25/WID_Assault_Chrono_RedDotAR_Athena_SR.WID_Assault_Chrono_RedDotAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotARS25/WID_Assault_Chrono_RedDotAR_Athena_VR.WID_Assault_Chrono_RedDotAR_Athena_VR"), 1));
    }

    if (SeasonIs(26))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_SR.WID_Assault_FlipMag_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_VR.WID_Assault_FlipMag_Athena_VR"), 1));
    }

    // Chapter 5 Season 1
    if (SeasonIs(28))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Heavy/WID_Assault_Paprika_Heavy_Athena_VR.WID_Assault_Paprika_Heavy_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Heavy/WID_Assault_Paprika_Heavy_Athena_SR.WID_Assault_Paprika_Heavy_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Heavy/WID_Assault_Paprika_Heavy_Athena_UR_Boss.WID_Assault_Paprika_Heavy_Athena_UR_Boss"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_DPS/WID_Assault_Paprika_DPS_Athena_VR.WID_Assault_Paprika_DPS_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_DPS/WID_Assault_Paprika_DPS_Athena_SR.WID_Assault_Paprika_DPS_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_DPS/WID_Assault_Paprika_DPS_Athena_UR_Boss.WID_Assault_Paprika_DPS_Athena_UR_Boss"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Infantry/WID_Assault_Paprika_Infantry_Athena_R.WID_Assault_Paprika_Infantry_Athena_R"), 1));

        // The Enforcer entered the live pool during the 28.01 line.
        if (VersionInfo.FortniteVersion >= 28.01)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Infantry/WID_Assault_Paprika_Infantry_Athena_VR.WID_Assault_Paprika_Infantry_Athena_VR"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Infantry/WID_Assault_Paprika_Infantry_Athena_SR.WID_Assault_Paprika_Infantry_Athena_SR"), 1));
        }
    }

    // Chapter 5 Season 2
    if (SeasonIs(29))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Heavy/WID_Assault_Paprika_Heavy_Athena_VR.WID_Assault_Paprika_Heavy_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Heavy/WID_Assault_Paprika_Heavy_Athena_SR.WID_Assault_Paprika_Heavy_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/WarforgedAR/WID_Assault_SunRose_Athena_VR.WID_Assault_SunRose_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/WarforgedAR/WID_Assault_SunRose_Athena_SR.WID_Assault_SunRose_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/WarforgedAR/WID_Assault_SunRose_Athena_UR.WID_Assault_SunRose_Athena_UR"), 1));

        if (VersionInfo.FortniteVersion >= 29.30)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/TacticalAR/WID_Assault_SunRose_Tactical_Athena_VR.WID_Assault_SunRose_Tactical_Athena_VR"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/TacticalAR/WID_Assault_SunRose_Tactical_Athena_SR.WID_Assault_SunRose_Tactical_Athena_SR"), 1));
        }
    }

    // Chapter 5 Season 3
    if (SeasonIs(30))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/TacticalAR/WID_Assault_SunRose_Tactical_Athena_VR.WID_Assault_SunRose_Tactical_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/TacticalAR/WID_Assault_SunRose_Tactical_Athena_SR.WID_Assault_SunRose_Tactical_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Infantry/WID_Assault_Paprika_Infantry_Athena_VR.WID_Assault_Paprika_Infantry_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaAR_Infantry/WID_Assault_Paprika_Infantry_Athena_SR.WID_Assault_Paprika_Infantry_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DurableDawnCarp/Gameplay/WID_DurableDawnCarp_HS.WID_DurableDawnCarp_HS"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/CombatAR/WID_Assault_MoonFlax_CombatAR_Athena_UR.WID_Assault_MoonFlax_CombatAR_Athena_UR"), 1));

        if (VersionInfo.FortniteVersion < 30.10)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/WarforgedAR/WID_Assault_SunRose_Athena_VR.WID_Assault_SunRose_Athena_VR"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/WarforgedAR/WID_Assault_SunRose_Athena_SR.WID_Assault_SunRose_Athena_SR"), 1));
        }
        else
        {
            // Ruckus was the only remaining source after the 30.10 vault.
            Slot1.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/WarforgedAR/WID_Assault_SunRose_Athena_R.WID_Assault_SunRose_Athena_R"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/CombatAR/WID_Assault_MoonFlax_CombatAR_Athena_VR.WID_Assault_MoonFlax_CombatAR_Athena_VR"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/CombatAR/WID_Assault_MoonFlax_CombatAR_Athena_SR.WID_Assault_MoonFlax_CombatAR_Athena_SR"), 1));
        }

        if (VersionInfo.FortniteVersion >= 30.40)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_R_Ore_T03.WID_Assault_LMG_Athena_R_Ore_T03"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_VR_Ore_T03.WID_Assault_LMG_Athena_VR_Ore_T03"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_SR_Ore_T03.WID_Assault_LMG_Athena_SR_Ore_T03"), 1));
        }
    }

    Slots.Add(Slot1);

    TArray<TPair<FString, int>> Slot2;

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 9.00) || VersionIn(9.30, 13.00) ||
        SeasonIs(14) || VersionIn(16.00, 19.00) || SeasonIs(27))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_C_Ore_T03.WID_Shotgun_Standard_Athena_C_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_UC_Ore_T03.WID_Shotgun_Standard_Athena_UC_Ore_T03"), 1));
    }

    if (VersionIn(6.31, 9.00) || VersionIn(9.30, 13.00) || SeasonIs(14) ||
        VersionIn(16.00, 19.00) || SeasonIs(27))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_VR_Ore_T03.WID_Shotgun_Standard_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_SR_Ore_T03.WID_Shotgun_Standard_Athena_SR_Ore_T03"), 1));
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 14.00) || SeasonIs(15) ||
        VersionIn(16.30, 18.00) || SeasonIs(27))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_R_Ore_T03.WID_Shotgun_SemiAuto_Athena_R_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_VR_Ore_T03.WID_Shotgun_SemiAuto_Athena_VR_Ore_T03"), 1));
    }

    if (VersionIn(3.31, 7.30) || VersionIs(9.30))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_VR.WID_Shotgun_SlugFire_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_SR.WID_Shotgun_SlugFire_Athena_SR"), 1));
    }

    if (VersionIn(5.20, 7.00) || VersionIs(9.30) || VersionIn(10.00, 10.31) || VersionIs(11.31) ||
        VersionIs(27.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_BreakBarrel_Athena_VR_Ore_T03.WID_Shotgun_BreakBarrel_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_BreakBarrel_Athena_SR_Ore_T03.WID_Shotgun_BreakBarrel_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(9.00, 10.22) || SeasonIs(14) || VersionIn(18.30, 19.00) ||
        VersionIn(23.00, 25.00) || VersionIs(27.10))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_R_Ore_T03.WID_Shotgun_Combat_Athena_R_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_VR_Ore_T03.WID_Shotgun_Combat_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_SR_Ore_T03.WID_Shotgun_Combat_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(9.30, 10.22) || VersionIs(11.31) || SeasonIs(20) || SeasonIs(25) || VersionIs(27.11))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_AutoDrum_Athena_UC_Ore_T03.WID_Shotgun_AutoDrum_Athena_UC_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_AutoDrum_Athena_R_Ore_T03.WID_Shotgun_AutoDrum_Athena_R_Ore_T03"), 1));
    }

    if (SeasonIs(20) || SeasonIs(25))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_AutoDrum_Athena_VR_Ore_T03.WID_Shotgun_AutoDrum_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_AutoDrum_Athena_SR_Ore_T03.WID_Shotgun_AutoDrum_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(9.40, 14.00) || SeasonIs(15) || VersionIn(16.30, 18.00) || SeasonIs(27))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(13.00, 14.40) || (std::floor(VersionInfo.FortniteVersion)) == 15 ||
        SeasonIs(18) || VersionIn(24.10, 25.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Charge_Athena_VR_Ore_T03.WID_Shotgun_Charge_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Charge_Athena_SR_Ore_T03.WID_Shotgun_Charge_Athena_SR_Ore_T03"), 1));
    }

    if (SeasonIs(13))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Charge_Athena_UR_Ore_T03.WID_Shotgun_Charge_Athena_UR_Ore_T03"), 1));
    }

    if (VersionIn(15.00, 16.40) || VersionIn(18.00, 23.40) || VersionIs(23.50))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_Dub.WID_WaffleTruck_Dub"), 1));
    }

    if (VersionIn(19.00, 22.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_UC.WID_Shotgun_CoreBurst_Athena_UC"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_R.WID_Shotgun_CoreBurst_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_VR.WID_Shotgun_CoreBurst_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_SR.WID_Shotgun_CoreBurst_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/DPSShotgun/WID_Shotgun_CoreDPS_Athena_VR.WID_Shotgun_CoreDPS_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/DPSShotgun/WID_Shotgun_CoreDPS_Athena_SR.WID_Shotgun_CoreDPS_Athena_SR"), 1));
    }

    if (VersionIn(19.20, 20.00) || VersionIn(23.10, 24.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Heavy_Athena_R.WID_Shotgun_Heavy_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Heavy_Athena_VR.WID_Shotgun_Heavy_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Heavy_Athena_SR.WID_Shotgun_Heavy_Athena_SR"), 1));
    }

    if (VersionIn(20.20, 21.00) || VersionIn(22.00, 23.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_SR_Ore_T03.WID_Shotgun_Break_Action_Athena_SR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_VR_Ore_T03.WID_Shotgun_Break_Action_Athena_VR_Ore_T03"), 1));
    }

    if (SeasonIs(21))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/TwoShotShotgun/WID_Shotgun_TwoShot_Pump_Athena_VR.WID_Shotgun_TwoShot_Pump_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/TwoShotShotgun/WID_Shotgun_TwoShot_Pump_Athena_SR.WID_Shotgun_TwoShot_Pump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/TwoShotShotgun/WID_Shotgun_TwoShot_Pump_Athena_UR.WID_Shotgun_TwoShot_Pump_Athena_UR"), 1));
    }

    if (VersionIn(21.30, 23.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_SR.WID_Shotgun_OverLoad_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_VR.WID_Shotgun_OverLoad_Athena_VR"), 1));

        if (VersionInfo.FortniteVersion >= 22.00)
        {
            Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_UR.WID_Shotgun_OverLoad_Athena_UR"), 1));
        }
    }

    if (SeasonIs(22))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Shotgun/WMID_Shotgun_Chrome_Athena_VR.WMID_Shotgun_Chrome_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Shotgun/WMID_Shotgun_Chrome_Athena_SR.WMID_Shotgun_Chrome_Athena_SR"), 1));
    }

    if (SeasonIs(23))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_SR.WID_Shotgun_MusterPump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_VR.WID_Shotgun_MusterPump_Athena_VR"), 1));
    }

    if (VersionIn(23.00, 27.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterDPSShotgun/WID_Shotgun_MusterDPS_Athena_SR.WID_Shotgun_MusterDPS_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterDPSShotgun/WID_Shotgun_MusterDPS_Athena_VR.WID_Shotgun_MusterDPS_Athena_VR"), 1));
    }

    if (VersionIn(24.00, 26.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_UR.WID_Shotgun_RadicalPump_Athena_UR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_SR.WID_Shotgun_RadicalPump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_VR.WID_Shotgun_RadicalPump_Athena_VR"), 1));
    }

    if (VersionIn(25.11, 27.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_SR.WID_Shotgun_Chrono_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_VR.WID_Shotgun_Chrono_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_R.WID_Shotgun_Chrono_Athena_R"), 1));
    }

    if (SeasonIs(26))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/HopscotchShotgun/WID_Shotgun_HopScotch_Athena_VR.WID_Shotgun_HopScotch_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/HopscotchShotgun/WID_Shotgun_HopScotch_Athena_SR.WID_Shotgun_HopScotch_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/HopscotchShotgun/WID_Shotgun_HopScotch_Athena_UR_MMO.WID_Shotgun_HopScotch_Athena_UR_MMO"), 1));
    }

    if (VersionIs(26.30))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/VampireStakeGameplay/Items/StakeLauncher/WID_VampireStake_Shotgun_R.WID_VampireStake_Shotgun_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/VampireStakeGameplay/Items/StakeLauncher/WID_VampireStake_Shotgun_VR.WID_VampireStake_Shotgun_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/VampireStakeGameplay/Items/StakeLauncher/WID_VampireStake_Shotgun_SR.WID_VampireStake_Shotgun_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 28.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_SR.WID_Shotgun_Pump_Paprika_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_VR.WID_Shotgun_Pump_Paprika_Athena_VR"), 1));

        if (SeasonIs(28))
        {
            Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_UR_Boss.WID_Shotgun_Pump_Paprika_Athena_UR_Boss"), 1));
        }
    }

    if (VersionIn(28.00, 30.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_SR.WID_Shotgun_Auto_Paprika_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_VR.WID_Shotgun_Auto_Paprika_Athena_VR"), 1));

        if (SeasonIs(28))
        {
            Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_UR_Boss.WID_Shotgun_Auto_Paprika_Athena_UR_Boss"), 1));
        }
    }

    if (VersionInfo.FortniteVersion >= 29.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_SR.WID_Shotgun_Break_Cerberus_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_VR.WID_Shotgun_Break_Cerberus_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_R.WID_Shotgun_Break_Cerberus_Athena_R"), 1));

        if (VersionIn(29.00, 31.00))
        {
            Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_UR.WID_Shotgun_Break_Cerberus_Athena_UR"), 1));
        }
    }

    if (SeasonIs(30))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_UR_Boss.WID_Shotgun_Auto_Paprika_Athena_UR_Boss"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/CombatShotgun/WID_Shotgun_Moonflax_Combat_Athena_VR.WID_Shotgun_Moonflax_Combat_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/CombatShotgun/WID_Shotgun_Moonflax_Combat_Athena_SR.WID_Shotgun_Moonflax_Combat_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/CombatShotgun/WID_Shotgun_Moonflax_Combat_Athena_UR.WID_Shotgun_Moonflax_Combat_Athena_UR"), 1));
    }

    Slots.Add(Slot2);

    // Slot 3 (Precision / Flex Weapons)
    TArray<TPair<FString, int>> Slot3;

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 7.30) || VersionIn(9.30, 12.00) ||
        VersionIn(13.00, 16.00) || VersionIn(17.00, 17.40) || VersionIs(17.50) || VersionIs(27.00))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03"), 1));

        if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 5.40) || VersionIn(9.30, 12.00) ||
            VersionIn(13.00, 16.00) || VersionIn(17.00, 17.40) || VersionIs(17.50) ||
            VersionIs(27.00))
        {
            Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03"), 1));
        }
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 6.21) || VersionIn(9.10, 10.00) || VersionIs(27.00))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Standard_Scope_Athena_VR_Ore_T03.WID_Sniper_Standard_Scope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Standard_Scope_Athena_SR_Ore_T03.WID_Sniper_Standard_Scope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(3.10, 9.20) || VersionIn(10.00, 11.00) || VersionIs(11.31) || SeasonIs(13) ||
        VersionIs(20.30) || VersionIn(27.00, 27.11))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_UC_Ore_T03.WID_Sniper_NoScope_Athena_UC_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_R_Ore_T03.WID_Sniper_NoScope_Athena_R_Ore_T03"), 1));
    }

    if (SeasonIs(13) || VersionIs(20.30) || VersionIn(27.00, 27.11))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(5.21, 11.00) || VersionIs(11.31) || SeasonIs(12) || VersionIn(20.10, 22.00) ||
        VersionIs(23.40) || VersionIn(24.00, 25.11) || VersionIs(27.11))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_VR_Ore_T03.WID_Sniper_Heavy_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_SR_Ore_T03.WID_Sniper_Heavy_Athena_SR_Ore_T03"), 1));

        if (VersionIn(20.10, 22.00) || VersionIs(23.40) || VersionIn(24.00, 25.11))
        {
            Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_R_Ore_T03.WID_Sniper_Heavy_Athena_R_Ore_T03"), 1));
        }
    }

    if (VersionIn(7.10, 9.40) || SeasonIs(12) || VersionIs(17.40) || SeasonIs(26) || VersionIs(27.1))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03"), 1));

        if (VersionIs(17.40))
        {
            Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03"), 1));
        }
    }

    if (SeasonIs(16))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Shockwave/WID_Bow_Shockwave_Athena_VR.WID_Bow_Shockwave_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Shockwave/WID_Bow_Shockwave_Athena_SR.WID_Bow_Shockwave_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Metal/WID_Bow_Metal_Athena_R.WID_Bow_Metal_Athena_R"), 1));

        if (VersionInfo.FortniteVersion >= 16.20)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Exotics/GrapplerBow/WID_Athena_Bow_Grappler.WID_Athena_Bow_Grappler"), 1));
        }

        if (VersionInfo.FortniteVersion >= 16.30)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Exotics/UnstableBow/WMID_Athena_UnstableBow.WMID_Athena_UnstableBow"), 1));
        }
    }

    if (VersionIn(9.41, 10.20))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Weather_Athena_VR.WID_Sniper_Weather_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Weather_Athena_SR.WID_Sniper_Weather_Athena_SR"), 1));
    }

    if (VersionIn(15.00, 16.00) || SeasonIs(17) || SeasonIs(20))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_Sniper_StormScout.WID_WaffleTruck_Sniper_StormScout"), 1));
    }

    if (VersionIn(15.10, 16.00) || VersionIn(18.00, 20.00) || VersionIn(24.00, 26))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_Sniper_DragonBreath.WID_WaffleTruck_Sniper_DragonBreath"), 1));
    }

    if (VersionIn(15.10, 16.00))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_UC.WID_Sniper_Cowboy_Athena_UC"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_R.WID_Sniper_Cowboy_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_VR.WID_Sniper_Cowboy_Athena_VR"), 1));
    }

    if (VersionIn(19.00, 21.00) || SeasonIs(22))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_UC.WID_Sniper_CoreSniper_Athena_UC"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_R.WID_Sniper_CoreSniper_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_VR.WID_Sniper_CoreSniper_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_SR.WID_Sniper_CoreSniper_Athena_SR"), 1));
    }

    if (VersionIn(25.11, 26.00))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ExplosiveRepeater/WID_Sniper_ExplosiveRepeater_Athena_SR.WID_Sniper_ExplosiveRepeater_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ExplosiveRepeater/WID_Sniper_ExplosiveRepeater_Athena_VR.WID_Sniper_ExplosiveRepeater_Athena_VR"), 1));
    }

    // Chapter 5 Season 1: Reaper plus the season's SMG/pistol flex choices.
    if (SeasonIs(28))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSniper/WID_Sniper_Paprika_Athena_VR.WID_Sniper_Paprika_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSniper/WID_Sniper_Paprika_Athena_SR.WID_Sniper_Paprika_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_DPS/WID_SMG_Paprika_DPS_Athena_VR.WID_SMG_Paprika_DPS_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_DPS/WID_SMG_Paprika_DPS_Athena_SR.WID_SMG_Paprika_DPS_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_DPS/WID_SMG_Paprika_DPS_Athena_UR_Boss.WID_SMG_Paprika_DPS_Athena_UR_Boss"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_Burst/WID_SMG_Paprika_Burst_Athena_VR.WID_SMG_Paprika_Burst_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_Burst/WID_SMG_Paprika_Burst_Athena_SR.WID_SMG_Paprika_Burst_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaPistol/WID_Pistol_Paprika_Auto_Athena_VR.WID_Pistol_Paprika_Auto_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaPistol/WID_Pistol_Paprika_Auto_Athena_SR.WID_Pistol_Paprika_Auto_Athena_SR"), 1));

        if (VersionInfo.FortniteVersion >= 28.01)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/Smartgun/Gameplay/StackingLockons/WID_Pistol_AutoAim.WID_Pistol_AutoAim"), 1));
        }
    }

    // Chapter 5 Season 2
    if (SeasonIs(29))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSniper/WID_Sniper_Paprika_Athena_VR.WID_Sniper_Paprika_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSniper/WID_Sniper_Paprika_Athena_SR.WID_Sniper_Paprika_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/ModularDMR/WID_SunRose_ModularDMR_Athena_R.WID_SunRose_ModularDMR_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/ModularDMR/WID_SunRose_ModularDMR_Athena_VR.WID_SunRose_ModularDMR_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/ModularDMR/WID_SunRose_ModularDMR_Athena_SR.WID_SunRose_ModularDMR_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/ModularDMR/WID_SunRose_ModularDMR_Athena_UR.WID_SunRose_ModularDMR_Athena_UR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/HarbingerSMG/WID_SMG_SunRose_DPS_Athena_VR.WID_SMG_SunRose_DPS_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/HarbingerSMG/WID_SMG_SunRose_DPS_Athena_SR.WID_SMG_SunRose_DPS_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/HarbingerSMG/WID_SMG_SunRose_DPS_Athena_UR_Boss.WID_SMG_SunRose_DPS_Athena_UR_Boss"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_Burst/WID_SMG_Paprika_Burst_Athena_VR.WID_SMG_Paprika_Burst_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_Burst/WID_SMG_Paprika_Burst_Athena_SR.WID_SMG_Paprika_Burst_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaPistol/WID_Pistol_Paprika_Auto_Athena_VR.WID_Pistol_Paprika_Auto_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaPistol/WID_Pistol_Paprika_Auto_Athena_SR.WID_Pistol_Paprika_Auto_Athena_SR"), 1));

        if (VersionInfo.FortniteVersion >= 29.01)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/DrumGun/WID_SunRose_DrumGun_Athena_VR.WID_SunRose_DrumGun_Athena_VR"), 1));
            Slot3.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/DrumGun/WID_SunRose_DrumGun_Athena_SR.WID_SunRose_DrumGun_Athena_SR"), 1));
        }

        if (VersionInfo.FortniteVersion >= 29.10)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/HandCannon/WID_SunRose_Pistol_HandCannon_Athena_VR.WID_SunRose_Pistol_HandCannon_Athena_VR"), 1));
            Slot3.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/HandCannon/WID_SunRose_Pistol_HandCannon_Athena_SR.WID_SunRose_Pistol_HandCannon_Athena_SR"), 1));
            Slot3.Add(TPair<FString, int>(TEXT("/KeepScoreGameplay/Items/TiredPanda/Blueprints/Lightweight/WID_TiredPanda_Lightweight.WID_TiredPanda_Lightweight"), 1));
        }

        if (VersionIs(29.40))
        {
            Slot3.Add(TPair<FString, int>(TEXT("/GuineaPig/Items/Crossbow/WID_GuineaPig_Crossbow_Charge_v1.WID_GuineaPig_Crossbow_Charge_v1"), 1));
        }
    }

    // Chapter 5 Season 3
    if (SeasonIs(30))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/ModularDMR/WID_SunRose_ModularDMR_Athena_R.WID_SunRose_ModularDMR_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/ModularDMR/WID_SunRose_ModularDMR_Athena_VR.WID_SunRose_ModularDMR_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/ModularDMR/WID_SunRose_ModularDMR_Athena_SR.WID_SunRose_ModularDMR_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/MoonFlaxWeaponGameplay/Items/Weapons/Crossbow/WID_Crossbow_MoonFlax_Athena_R.WID_Crossbow_MoonFlax_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/MoonFlaxWeaponGameplay/Items/Weapons/Crossbow/WID_Crossbow_MoonFlax_Athena_VR.WID_Crossbow_MoonFlax_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/MoonFlaxWeaponGameplay/Items/Weapons/Crossbow/WID_Crossbow_MoonFlax_Athena_SR.WID_Crossbow_MoonFlax_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/MoonFlaxWeaponGameplay/Items/Weapons/Crossbow/WID_Crossbow_MoonFlax_Athena_UR.WID_Crossbow_MoonFlax_Athena_UR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/HarbingerSMG/WID_SMG_SunRose_DPS_Athena_VR.WID_SMG_SunRose_DPS_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/HarbingerSMG/WID_SMG_SunRose_DPS_Athena_SR.WID_SMG_SunRose_DPS_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_Burst/WID_SMG_Paprika_Burst_Athena_VR.WID_SMG_Paprika_Burst_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaSMG_Burst/WID_SMG_Paprika_Burst_Athena_SR.WID_SMG_Paprika_Burst_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaPistol/WID_Pistol_Paprika_Auto_Athena_VR.WID_Pistol_Paprika_Auto_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaPistol/WID_Pistol_Paprika_Auto_Athena_SR.WID_Pistol_Paprika_Auto_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/HandCannon/WID_SunRose_Pistol_HandCannon_Athena_VR.WID_SunRose_Pistol_HandCannon_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/HandCannon/WID_SunRose_Pistol_HandCannon_Athena_SR.WID_SunRose_Pistol_HandCannon_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/WeaponsUpdated/Gameplay/HandCannon/WID_SunRose_Pistol_HandCannon_Athena_UR.WID_SunRose_Pistol_HandCannon_Athena_UR"), 1));

        if (VersionInfo.FortniteVersion >= 30.20)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/MoonFlaxWeaponGameplay/Items/Weapons/Sniper/WID_Sniper_MoonFlax_Athena_R.WID_Sniper_MoonFlax_Athena_R"), 1));
            Slot3.Add(TPair<FString, int>(TEXT("/MoonFlaxWeaponGameplay/Items/Weapons/Sniper/WID_Sniper_MoonFlax_Athena_VR.WID_Sniper_MoonFlax_Athena_VR"), 1));
            Slot3.Add(TPair<FString, int>(TEXT("/MoonFlaxWeaponGameplay/Items/Weapons/Sniper/WID_Sniper_MoonFlax_Athena_SR.WID_Sniper_MoonFlax_Athena_SR"), 1));
            Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_UC.WID_Pistol_Flintlock_Athena_UC"), 1));

            if (VersionInfo.FortniteVersion < 30.30)
            {
                Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Auto_Athena_C.WID_Pistol_Auto_Athena_C"), 1));
            }
        }
    }

    Slots.Add(Slot3);

    // Slot 4 (Heals)
    TArray<TPair<FString, int>> Slot4;

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 28.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Medkit/Athena_Medkit.Athena_Medkit"), 3));
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 28.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Shields/Athena_Shields.Athena_Shields"), 3));
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.80, 11.00) || VersionIs(23.30) || VersionIn(24.00, 27.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff"), 2));
    }

    if (VersionIn(1.11, 28.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShieldSmall/Athena_ShieldSmall.Athena_ShieldSmall"), 6));
    }

    if (VersionIn(2.30, 11.00) || SeasonIs(27))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SuperMedkit/Athena_SuperMedkit.Athena_SuperMedkit"), 1));
    }

    if (VersionIn(9.30, 11.00) || VersionIn(13.00, 16.00) || VersionIn(18.10, 25.00) ||
        VersionIn(26.10, 27.00) || VersionIs(27.11) || SeasonIs(29))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ChillBronco/Athena_ChillBronco.Athena_ChillBronco"), 6));
    }

    if (VersionIn(29.10, 30.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/SunRoseConsumablesGameplay/Gameplay/GoldenFruit/WID_Athena_GoldenFruit.WID_Athena_GoldenFruit"), 4));
    }

    if (VersionIn(11.00, 27.00) || (VersionInfo.FortniteVersion >= 28.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/WID_Athena_Flopper.WID_Athena_Flopper"), 4));
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/Effective/WID_Athena_Flopper_Effective.WID_Athena_Flopper_Effective"), 3));
    }

    if (SeasonIs(13))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/BottomlessChugJug/WID_Athena_BottomlessChugJug.WID_Athena_BottomlessChugJug"), 1));
    }

    if (VersionIn(14.00, 15.00) || VersionIn(16.00, 18.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/HopFlopper/WID_Athena_Flopper_HopFlopper.WID_Athena_Flopper_HopFlopper"), 3));
    }

    if (VersionIn(15.00, 16.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/RiftFlopper/WID_Athena_Flopper_Rift.WID_Athena_Flopper_Rift"), 2));

        if (VersionInfo.FortniteVersion >= 15.10)
        {
            Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/ZeroFlopper/WID_Athena_Flopper_Zero.WID_Athena_Flopper_Zero"), 3));
        }
    }

    if (VersionIn(19.00, 27.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/HealSpray/WID_Athena_HealSpray.WID_Athena_HealSpray"), 1));
    }

    if (VersionIn(23.00, 26.10))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/MusterConsumables/Items/EnergyDrink/Items/WID_Athena_EnergyDrink.WID_Athena_EnergyDrink"), 6));
    }

    if (VersionInfo.FortniteVersion >= 28.00) // paprika items
    {
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/BandageRefresh/WID_Paprika_Bandage.WID_Paprika_Bandage"), 15));
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/ShieldSmallRefresh/WID_Paprika_ShieldSmall.WID_Paprika_ShieldSmall"), 6));
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/ShieldRefresh/WID_Paprika_ShieldPot.WID_Paprika_ShieldPot"), 3));
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/MedRefresh/WID_Paprika_MedBox.WID_Paprika_MedBox"), 3));
    }

    if (VersionInfo.FortniteVersion >= 28.01)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/TeamSpray/WID_Paprika_TeamSpray_LowGrav.WID_Paprika_TeamSpray_LowGrav"), 1));
    }

    if (SeasonIs(30))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/DurableDawnTuna/Gameplay/WID_DurableDawnTuna.WID_DurableDawnTuna"), 2));
    }

    if (VersionIn(30.30, 31.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/HealSpray/WID_Athena_HealSpray.WID_Athena_HealSpray"), 1));
    }

    const bool bChapter5VersionizedLoadout = VersionIn(28.00, 31.00);
    if (!bChapter5VersionizedLoadout)
        Slots.Add(Slot4);

    // Utility pool (slot 4 in Chapter 5, slot 5 in earlier seasons).
    TArray<TPair<FString, int>> Slot5;

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 11.10) || VersionIn(11.11, 12.00))
    {
        if (VersionInfo.FortniteVersion < 5.40 || VersionInfo.FortniteVersion >= 11.00)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_R_Ore_T03.WID_Launcher_Rocket_Athena_R_Ore_T03"), 1));
        }

        if (VersionInfo.FortniteVersion >= 11.00)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_UC_Ore_T03.WID_Launcher_Rocket_Athena_UC_Ore_T03"), 1));
        }
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 11.10) || VersionIn(11.11, 14.40) ||
        VersionIn(14.50, 18.21) || VersionIn(18.30, 19.00) || SeasonIs(23) || SeasonIs(27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_VR_Ore_T03.WID_Launcher_Rocket_Athena_VR_Ore_T03"), 1));

        if (VersionInfo.FortniteVersion != 14.00 || VersionInfo.FortniteVersion != 14.20) // dude like why
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_SR_Ore_T03.WID_Launcher_Rocket_Athena_SR_Ore_T03"), 1));
        }
    }

    if (VersionIs(11.10) || VersionIs(14.40) || VersionIs(18.21) || VersionIs(22.20) || VersionIs(26.30))
    {
        if (VersionIs(18.21))
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_VR_Ore_T03.WID_Launcher_Pumpkin_Athena_VR_Ore_T03"), 1));
        }
        else
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_R_Ore_T03.WID_Launcher_Pumpkin_Athena_R_Ore_T03"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_VR_Ore_T03.WID_Launcher_Pumpkin_Athena_VR_Ore_T03"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_SR_Ore_T03.WID_Launcher_Pumpkin_Athena_SR_Ore_T03"), 1));

            if (VersionIs(14.40))
            {
                Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_UC_Ore_T03.WID_Launcher_Pumpkin_Athena_UC_Ore_T03"), 1));
            }
        }
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 11.00) || VersionIn(19.10, 20.00) || SeasonIs(27))
    {
        if (VersionInfo.FortniteVersion < 19.10 || SeasonIs(27))
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_R_Ore_T03.WID_Launcher_Grenade_Athena_R_Ore_T03"), 1));
        }

        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_VR_Ore_T03.WID_Launcher_Grenade_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_SR_Ore_T03.WID_Launcher_Grenade_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIs(11.3) || VersionIs(19.01) || VersionIs(23.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Snowball_Athena_R_Ore_T03.WID_Launcher_Snowball_Athena_R_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Snowball_Athena_VR_Ore_T03.WID_Launcher_Snowball_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Snowball_Athena_SR_Ore_T03.WID_Launcher_Snowball_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIs(16.10) || VersionIs(20.10) || VersionIs(24.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Egg_Athena_R_Ore_T03.WID_Launcher_Egg_Athena_R_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Egg_Athena_VR_Ore_T03.WID_Launcher_Egg_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Egg_Athena_SR_Ore_T03.WID_Launcher_Egg_Athena_SR_Ore_T03"), 1));
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 7.40) || VersionIn(9.00, 26.00) || SeasonIs(27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Grenade/Athena_Grenade.Athena_Grenade"), 6));
    }

    if (VersionIn(2.5, 6.00) || VersionIn(8.11, 9.30) || VersionIn(21.30, 22.00) || SeasonIs(23) ||
        VersionIs(27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/KnockGrenade/Athena_KnockGrenade.Athena_KnockGrenade"), 9));
    }

    if (VersionIn(3.00, 11.00) || VersionIs(11.31) || VersionIs(15.40) || SeasonIs(17) ||
        VersionIs(23.40) || SeasonIs(27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_HandCannon_Athena_VR_Ore_T03.WID_Pistol_HandCannon_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_HandCannon_Athena_SR_Ore_T03.WID_Pistol_HandCannon_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIs(3.40) || VersionIn(5.10, 6.21))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_VR_T03.WID_RC_Rocket_Athena_VR_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_SR_T03.WID_RC_Rocket_Athena_SR_T03"), 1));
    }

    static std::mt19937 FixedRNG(std::random_device{}());
    std::uniform_int_distribution<int> Dist(1, 100);

    if (VersionIn(11.00, 19.00))
    {
        int Roll = Dist(FixedRNG);

        if (Roll == 1)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Bucket/Nice/WID_Athena_Bucket_Nice.WID_Athena_Bucket_Nice"), 1));
        }
    }

    if (VersionIn(3.30, 6.00) || SeasonIs(12) || SeasonIs(20) || VersionIn(26, 28.00) ||
        VersionIn(30.20, 31.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/C4/Athena_C4.Athena_C4"), 6));
    }

    if (VersionIn(1.11, 11.00) || VersionIs(11.31) || SeasonIs(14) || VersionIs(20.20) ||
        VersionIn(21.30, 22.10) || VersionIs(27.00) || VersionIs(30.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/DanceGrenade/Athena_DanceGrenade.Athena_DanceGrenade"), 3));
    }

    if (VersionIn(3.5, 7.00) || SeasonIs(14) || VersionIn(21.20, 22.00) || VersionIs(27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/TowerGrenade/Athena_TowerGrenade.Athena_TowerGrenade"), 2));
    }

    if (VersionIn(5.30, 9.30) || VersionIs(17.40) || VersionIs(18.00) || VersionIn(20.20, 22.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item"), 2));
    }

    if (VersionIn(5.30, 7.00) || VersionIn(9.00, 11.00) || VersionIs(11.31) ||
        VersionIn(14.00, 17.00) || VersionIn(20.00, 23.00) || VersionInfo.FortniteVersion >= 26.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade"), 6));
    }

    if (VersionIn(5.40, 7.20) || VersionIn(18.30, 19.00) || SeasonIs(27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Hook_Gun_VR_Ore_T03.WID_Hook_Gun_VR_Ore_T03"), 1));
    }

    if (VersionIn(5.41, 7.20) || VersionIs(27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SuperTowerGrenade/Levels/PortAFort_A/Athena_SuperTowerGrenade_A.Athena_SuperTowerGrenade_A"), 1));
    }

    if (VersionIn(6.02, 7.20) || VersionIs(9.30) || VersionIs(27.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_VR_Ore_T03.WID_Launcher_Military_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_SR_Ore_T03.WID_Launcher_Military_Athena_SR_Ore_T03"), 1));
    }

    if (VersionIn(6.20, 7.10) || VersionIs(9.30) || VersionIn(10.00, 10.31) || VersionIs(23.50) ||
        VersionIs(27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Revolver_SingleAction_Athena_UC.WID_Pistol_Revolver_SingleAction_Athena_UC"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Revolver_SingleAction_Athena_R.WID_Pistol_Revolver_SingleAction_Athena_R"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Revolver_SingleAction_Athena_VR.WID_Pistol_Revolver_SingleAction_Athena_VR"), 1));
    }

    if (VersionIn(6.21, 7.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Balloons/Athena_Balloons.Athena_Balloons"), 20));
    }

    if (VersionIn(7.00, 9.00) || VersionIn(20.30, 21.00) || VersionIs(22.00) ||
        VersionIn(26.10, 26.30) || VersionIs(27.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Balloons/Athena_Balloons_Consumable.Athena_Balloons_Consumable"), 10));
    }

    if (VersionIn(7.30, 8.00) || VersionIs(19.01))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade"), 6));
    }

    if (VersionIn(8.11, 10.00) || VersionIs(10.4) || VersionIs(11.31) || VersionIs(15.40) ||
        VersionIs(18.4) || VersionIs(20.30) || VersionIn(24.10, 25.00) || VersionIs(27.1))
    {
        if (VersionIn(24.10, 25.00))
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_UC.WID_Pistol_Flintlock_Athena_UC"), 1));
        }
        else
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_C.WID_Pistol_Flintlock_Athena_C"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_UC.WID_Pistol_Flintlock_Athena_UC"), 1));
        }
    }

    if (VersionIn(8.20, 9.30) || VersionIs(11.31))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_ExplosiveBow_Athena_SR.WID_ExplosiveBow_Athena_SR"), 1));
    }

    if (VersionIn(9.20, 10.20) || VersionIs(27.11))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/DeployableStorm/Athena_DogSweater.Athena_DogSweater"), 1));
    }

    if (VersionIn(10.20, 11.00) || VersionIs(20.30) || VersionIs(21.50) ||
        VersionIn(22.10, 23.00) || SeasonIs(24))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2"), 2));
    }

    if (VersionIs(10.40))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Badger_Grape_VR.WID_Badger_Grape_VR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Prototype/Badger_Bangs/WID_Athena_BadgerBangsNew.WID_Athena_BadgerBangsNew"), 10));
    }

    if (VersionIn(11.00, 27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Bucket/WID_Athena_Bucket_Old.WID_Athena_Bucket_Old"), 4));
    }

    if (SeasonIs(12))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH"), 1));
    }

    if (VersionIn(12.30, 12.60))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Mantis/Items/UncleBrolly/WID_UncleBrolly.WID_UncleBrolly"), 1));
    }

    if (VersionIs(11.3) || VersionIs(12.50) || VersionIs(20.30))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Kayak.WID_Athena_Galileo_Lobster_Kayak"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Limo.WID_Athena_Galileo_Lobster_Limo"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Moped.WID_Athena_Galileo_Lobster_Moped"), 1));

        if (VersionInfo.FortniteVersion != 20.30)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Rocket.WID_Athena_Galileo_Lobster_Rocket"), 1));
        }
        else
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Noble.WID_Athena_Galileo_Lobster_Noble"), 1));
        }
    }

    if (VersionIn(12.30, 15.00) || VersionIn(21.30, 22.00) || VersionIn(22.20, 23.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun"), 6));
    }

    if (SeasonIs(13))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_GrapplingHoot.WID_Boss_GrapplingHoot"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Shockwave_Athena_UR_Ore_T03.WID_Launcher_Shockwave_Athena_UR_Ore_T03"), 1));
    }

    if (SeasonIs(14))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Repulsors/CoreBR/WID_HighTower_Tomato_Repulsor_CoreBR.WID_HighTower_Tomato_Repulsor_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/RepulsorCannon/CoreBR/WID_HighTower_Tomato_RepulsorCannon_CoreBR.WID_HighTower_Tomato_RepulsorCannon_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Date/ChainLightning/CoreBR/WID_HighTower_Date_ChainLightning_CoreBR.WID_HighTower_Date_ChainLightning_CoreBR"), 1));
    }

    if (VersionIs(15.50) || VersionIs(17.30))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/LTM/Builder/Gameplay/Yeetknock_pistol/Builder_WID_YEETknock_UR.Builder_WID_YEETknock_UR"), 1));
    }

    if (VersionIs(19.30) || VersionIn(23.00, 23.40) || VersionIs(23.50))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Shockwave/WID_Bow_Shockwave_Athena_VR.WID_Bow_Shockwave_Athena_VR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Shockwave/WID_Bow_Shockwave_Athena_SR.WID_Bow_Shockwave_Athena_SR"), 1));
    }

    if (VersionIn(17.00, 19.00) || VersionIs(20.40))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/WID_Athena_Mother_Scooter.WID_Athena_Mother_Scooter"), 1));
    }

    if (SeasonIs(18))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Weapons/Melee/Scythe/WID_PowerUp_Scythe_VR.WID_PowerUp_Scythe_VR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Weapons/Melee/Scythe/WID_PowerUp_Scythe_SR.WID_PowerUp_Scythe_SR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Weapons/Melee/Scythe/WID_PowerUp_Scythe_UR.WID_PowerUp_Scythe_UR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Shockwave_Athena_SR.WID_Launcher_Shockwave_Athena_SR"), 1));
    }

    if (VersionIn(18.30, 19.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Consumables/IcyGrapple/WID_Athena_IcyGrapple.WID_Athena_IcyGrapple"), 1));
    }

    if (SeasonIs(19))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ParallelGameplay/Items/WestSausage/WID_WestSausage_Parallel.WID_WestSausage_Parallel"), 1));
    }

    if (VersionIn(19.01, 25.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/ShieldGenerator/WID_Athena_ShieldGenerator.WID_Athena_ShieldGenerator"), 2));
    }

    if (SeasonIs(20) || VersionIn(28.10, 29.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/HomingRocket/WIDs/WID_Launcher_HomingRocket_Athena_R.WID_Launcher_HomingRocket_Athena_R"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/HomingRocket/WIDs/WID_Launcher_HomingRocket_Athena_VR.WID_Launcher_HomingRocket_Athena_VR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/HomingRocket/WIDs/WID_Launcher_HomingRocket_Athena_SR.WID_Launcher_HomingRocket_Athena_SR"), 1));
    }

    if (SeasonIs(21) || VersionIs(22.10) || VersionIn(25.20, 26.00) || SeasonIs(27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/GrappleGloves/Items/GrappleGloves/WID_GrappleGloves.WID_GrappleGloves"), 1));
    }

    if (VersionIn(13.20, 14.00) || VersionIs(16.40) || VersionIn(19.01, 20.00) ||
        VersionIn(24.30, 25.00) || VersionIs(25.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Prototype/WID_FringePlank_Athena_Prototype.WID_FringePlank_Athena_Prototype"), 1));
    }

    if (VersionIn(21.10, 22.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/SawbladeGun/Items/SawbladeGun/WID_Sawblade_Athena.WID_Sawblade_Athena"), 1));
    }

    if (VersionIs(21.10) || VersionIs(22.00) || VersionIs(25.11))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Fringeplank_Firework/Items/WIDs/WID_Firework_Gun.WID_Firework_Gun"), 1));
    }

    if (VersionIn(22.10, 23.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/UnstableLiquidGun/Items/UnstableLiquidGun/WID_NitroglycerineGun.WID_NitroglycerineGun"), 1));
    }

    if (VersionIn(22.10, 24.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/LaunchPadItemGameplay/Items/WID/WID_Athena_LaunchPadThrown.WID_Athena_LaunchPadThrown"), 2));
    }

    if (VersionIs(22.40))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/GrappleGlider/Items/WID_GrappleGlider.WID_GrappleGlider"), 1));
    }

    if (SeasonIs(23))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Exotics/GrapplerBow/WID_Athena_Bow_Grappler.WID_Athena_Bow_Grappler"), 1));
    }

    if (VersionIn(23.00, 23.50))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ShockwaveMace/Items/WID_Muster_ShockwaveMace.WID_Muster_ShockwaveMace"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ShockwaveMace/Items/WID_Muster_ShockwaveMace_Mythic.WID_Muster_ShockwaveMace_Mythic"), 1));
    }

    if (VersionIn(24.00, 24.40))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/KatanaGameplay/Items/Katana/WID_Melee_Katana.WID_Melee_Katana"), 1));
    }

    if (VersionIs(24.20))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/DryBox/Items/NyxGlass/AGID_NyxGlass.AGID_NyxGlass"), 1));
    }

    if (SeasonIs(25))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/AncientWeapon/Gameplay/WID_Chrono_AncientWeapon.WID_Chrono_AncientWeapon"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/AloeCroutonWeapon/Items/AloeCrouton/AGID_AloeCrouton_Launcher_Athena.AGID_AloeCrouton_Launcher_Athena"), 1));
    }

    if (VersionIn(25.10, 26.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ChronoCloak/Cloak/WID_Chrono_Cloak.WID_Chrono_Cloak"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ChronoCloak/Cloak/WID_Chrono_Cloak_UR_JungBoss.WID_Chrono_Cloak_UR_JungBoss"), 1));
    }

    if (SeasonIs(26) || VersionIn(28.01, 29.00) || VersionIn(30.20, 31.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/Proto/WID_Athena_AppleSunSmall.WID_Athena_AppleSunSmall"), 6));
    }

    if (SeasonIs(26))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/RocketRamGameplay/Items/RocketRam/WID_RocketRam.WID_RocketRam"), 1));
    }

    if (VersionIs(26.30))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/VampireKatanaGameplay/Items/VampireKatana/WID_Melee_Vampire_Katana.WID_Melee_Vampire_Katana"), 1));
    }

    // Chapter 5 Season 1 utility.
    if (SeasonIs(28))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ClusterBombGameplay/Gameplay/WID_Paprika_ClusterBomb.WID_Paprika_ClusterBomb"), 4));
        Slot5.Add(TPair<FString, int>(TEXT("/EMPGameplay/Items/EMPGrenade/WID_Athena_Grenade_EMP.WID_Athena_Grenade_EMP"), 6));
        Slot5.Add(TPair<FString, int>(TEXT("/BallisticShieldGameplay/Gameplay/WID_Paprika_BallisticShield.WID_Paprika_BallisticShield"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/GrappleWeapon/Items/GrappleKnife/V2/WID_GrappleKnife_VR.WID_GrappleKnife_VR"), 1));

        if (VersionInfo.FortniteVersion >= 28.10)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/CleanSweepChart/Gameplay/Items/AGID_CleanSweepChart.AGID_CleanSweepChart"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/CleanSweepDiagram/Gameplay/WID_CleanSweepDiagram.WID_CleanSweepDiagram"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/DeployableTurretGameplay/Items/DeployableTurret/WID_Athena_Sentry_Turret_Deployable.WID_Athena_Sentry_Turret_Deployable"), 2));
        }
    }

    if (VersionIn(29.00, 29.20) || VersionIn(29.40, 30.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/SunRoseFlyingGameplay/Items/Wings/WID_Athena_SunRose_Wings.WID_Athena_SunRose_Wings"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/SunRoseZeusGameplay/Items/Lightning/WID_Athena_SunRose_Zeus_Lightning.WID_Athena_SunRose_Zeus_Lightning"), 1));
    }

    if (VersionIn(29.01, 29.20) || VersionIn(29.40, 30.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/SunRoseChainWhipGameplay/Items/ChainWhip/WID_ChainWhip.WID_ChainWhip"), 1));
    }

    if (VersionIn(29.00, 29.40))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ClusterBombGameplay/Gameplay/WID_Paprika_ClusterBomb.WID_Paprika_ClusterBomb"), 4));
        Slot5.Add(TPair<FString, int>(TEXT("/EMPGameplay/Items/EMPGrenade/WID_Athena_Grenade_EMP.WID_Athena_Grenade_EMP"), 6));
    }

    if (VersionIn(29.20, 29.40))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HumbleMop/Blueprints/WID_HumbleMop.WID_HumbleMop"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/FancyMop/Blueprints/WID_FancyMop.WID_FancyMop"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/AnnoyedMop/Gameplay/WID_AnnoyedMop.WID_AnnoyedMop"), 1));
    }

    if (VersionIn(29.40, 30.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/BigBushGrenade/Items/Athena_Player_BushBomb.Athena_Player_BushBomb"), 4));
    }

    // Chapter 5 Season 3 utility.
    if (SeasonIs(30))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/NitroGauntletsGameplay/Gameplay/WID_Moonflax_NitroGauntlet.WID_Moonflax_NitroGauntlet"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/NitroGauntletsGameplay/Gameplay/WID_Moonflax_NitroGauntlet_Mythic.WID_Moonflax_NitroGauntlet_Mythic"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/MoonflaxNitroSplash/Gameplay/WID_Moonflax_NitroSplash.WID_Moonflax_NitroSplash"), 6));

        if (VersionInfo.FortniteVersion >= 30.10)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/TowHookWeapon/Gameplay/WID_Athena_TowHookWeapon.WID_Athena_TowHookWeapon"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/EMPGameplay/Items/EMPGrenade/WID_Athena_Grenade_EMP.WID_Athena_Grenade_EMP"), 6));
        }

        if (VersionInfo.FortniteVersion >= 30.20)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/TwinStretchCorn/Gameplay/WID_TwinStretchCorn.WID_TwinStretchCorn"), 1));
        }

        if (VersionIn(30.20, 30.40))
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/Booty/AGID_Athena_Booty.AGID_Athena_Booty"), 1));
        }
    }

    if (VersionInfo.FortniteVersion >= 29.01)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ShieldBubble/Gameplay/ShieldBubbleJr/Athena_SilverBlazer_Mini_UC.Athena_SilverBlazer_Mini_UC"), 4));
    }

    if (bChapter5VersionizedLoadout)
    {
        Slots.Add(Slot5);
        Slots.Add(Slot4);
    }
    else
    {
        Slots.Add(Slot5);
    }

    // Slot 6 (Traps)
    TArray<TPair<FString, int>> Traps;

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.9, 11.00) || VersionIn(11.50, 14.00) ||
        VersionIn(17.00, 21.00) || SeasonIs(27))
    {
        Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Floor_Player_Launch_Pad_Athena.TID_Floor_Player_Launch_Pad_Athena"), 2));
    }

    if (VersionIn(4.30, 6.00) || VersionIs(10.40) || VersionIn(14.00, 16.00) || VersionIs(16.40) ||
        VersionIs(17.40) || VersionIs(19.40) || SeasonIs(27))
    {
        Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_BouncePad_Athena.TID_Context_BouncePad_Athena"), 3));
    }

    if (bSeasonOneDoubleDigitPatch || VersionIn(1.6, 12.00) || SeasonIs(27))
    {
        Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_ContextTrap_Athena.TID_ContextTrap_Athena"), 2));
    }

    if (VersionIn(18.00, 23.00))
    {
        Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_Reinforced_Athena.TID_Context_Reinforced_Athena"), 2));
    }

    Slots.Add(Traps);

    // Slot 7 (Materials)
    std::uniform_int_distribution<int> Mats(186, 646);
    std::uniform_int_distribution<int> Gold(1200, 7500);

    TArray<TPair<FString, int>> Materials;
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/WoodItemData.WoodItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/StoneItemData.StoneItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/MetalItemData.MetalItemData"), Mats(rng)));

    if (VersionInfo.FortniteVersion >= 15)
    {
        Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/Athena_WadsItemData.Athena_WadsItemData"), Gold(rng)));
    }

    Slots.Add(Materials);

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 186);
    std::uniform_int_distribution<int> Shells(87, 576);
    std::uniform_int_distribution<int> Medium(124, 824);
    std::uniform_int_distribution<int> Light(186, 824);
    std::uniform_int_distribution<int> STW(186, 999);
    std::uniform_int_distribution<int> Arrows(12, 30);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells"), Shells(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium"), Medium(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight"), Light(rng)));

    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AmmoDataRockets.AmmoDataRockets"), 12));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataExplosive.AmmoDataExplosive"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataEnergyCell.AmmoDataEnergyCell"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsHeavy.AmmoDataBulletsHeavy"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataShells.AmmoDataShells"), STW(rng)));

    if (VersionInfo.FortniteVersion >= 16.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Ammo/AthenaAmmoDataArrows.AthenaAmmoDataArrows"), Arrows(rng)));
    }

    Slots.Add(Ammo);

    return Slots;
}

TArray<TArray<TPair<FString, int>>> LateGame::GetOSLoadout()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    TArray<TArray<TPair<FString, int>>> Slots;

    // Slot 1 (Huntings)
    TArray<TPair<FString, int>> Slot1;
    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_UC_Ore_T03.WID_Sniper_NoScope_Athena_UC_Ore_T03"), 1));
    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_R_Ore_T03.WID_Sniper_NoScope_Athena_R_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 12)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03"), 1));
    }

    Slots.Add(Slot1);

    // Slot 2 (No-Scope Sniper)
    TArray<TPair<FString, int>> Slot2;
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 5.21)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_VR_Ore_T03.WID_Sniper_Heavy_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_SR_Ore_T03.WID_Sniper_Heavy_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 7.10)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 11.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_UC_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_UC_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.10)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_UC.WID_Sniper_Cowboy_Athena_UC"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_R.WID_Sniper_Cowboy_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_VR.WID_Sniper_Cowboy_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_UC.WID_Sniper_CoreSniper_Athena_UC"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_R.WID_Sniper_CoreSniper_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_VR.WID_Sniper_CoreSniper_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_SR.WID_Sniper_CoreSniper_Athena_SR"), 1));
    }

    Slots.Add(Slot2);

    // Slot 3 (Grappler)
    TArray<TPair<FString, int>> Slot3;

    if (VersionInfo.FortniteVersion >= 5.40)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Hook_Gun_VR_Ore_T03.WID_Hook_Gun_VR_Ore_T03"), 1));
    }

    if (VersionIs(10.40))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Badger_Grape_VR.WID_Badger_Grape_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH"), 1));
    }

    Slots.Add(Slot3);

    // Slot 4 (Consumables + Bandage)
    TArray<TPair<FString, int>> Slot4;

    Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/KnockGrenade/Athena_KnockGrenade.Athena_KnockGrenade"), 9));
    Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Bandage/Athena_Bandage.Athena_Bandage"), 15));

    if (VersionInfo.FortniteVersion >= 5.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item"), 2));
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 7.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 10.20)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2"), 2));
    }

    if (VersionInfo.FortniteVersion >= 12.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun"), 6));
    }

    Slots.Add(Slot4);

    // Slot 4 (Consumables)
    TArray<TPair<FString, int>> Slot5;

    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/KnockGrenade/Athena_KnockGrenade.Athena_KnockGrenade"), 9));

    if (VersionInfo.FortniteVersion >= 5.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item"), 2));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 7.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 10.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2"), 2));
    }

    if (VersionInfo.FortniteVersion >= 12.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun"), 6));
    }

    Slots.Add(Slot5);

    // Slot 6 (Traps)
    TArray<TPair<FString, int>> Traps;
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Floor_Player_Launch_Pad_Athena.TID_Floor_Player_Launch_Pad_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_BouncePad_Athena.TID_Context_BouncePad_Athena"), 3));

    Slots.Add(Traps);

    // Slot 7 (Materials)
    std::uniform_int_distribution<int> Gold(1200, 7500);

    TArray<TPair<FString, int>> Materials;
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/WoodItemData.WoodItemData"), 70));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/StoneItemData.StoneItemData"), 50));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/MetalItemData.MetalItemData"), 30));

    if (VersionInfo.FortniteVersion >= 15)
    {
        Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/Athena_WadsItemData.Athena_WadsItemData"), Gold(rng)));
    }

    Slots.Add(Materials);

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 186);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));

    Slots.Add(Ammo);

    return Slots;
}

TArray<TArray<TPair<FString, int>>> LateGame::GetCustomLoadout()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    TArray<TArray<TPair<FString, int>>> Slots;

    TArray<TPair<FString, int>> Slot1;

    auto PrimaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Primary.CStr());

    if (!PrimaryItemDefinition)
        PrimaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Primary).ToUtf8().c_str());

    if (PrimaryItemDefinition)
    {
        FString PrimaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)PrimaryItemDefinition);
        Slot1.Add(TPair<FString, int>(PrimaryFullPath, FConfiguration::PrimaryAmount));
    }

    Slots.Add(Slot1);

    TArray<TPair<FString, int>> Slot2;

    auto SecondaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Secondary.CStr());

    if (!SecondaryItemDefinition)
        SecondaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Secondary).ToUtf8().c_str());

    if (SecondaryItemDefinition)
    {
        FString SecondaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)SecondaryItemDefinition);
        Slot2.Add(TPair<FString, int>(SecondaryFullPath, FConfiguration::SecondaryAmount));
    }

    Slots.Add(Slot2);

    TArray<TPair<FString, int>> Slot3;

    auto TertiaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Tertiary.CStr());

    if (!TertiaryItemDefinition)
        TertiaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Tertiary).ToUtf8().c_str());

    if (TertiaryItemDefinition)
    {
        FString TertiaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)TertiaryItemDefinition);
        Slot3.Add(TPair<FString, int>(TertiaryFullPath, FConfiguration::TertiaryAmount));
    }

    Slots.Add(Slot3);

    TArray<TPair<FString, int>> Slot4;

    auto QuaternaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Quaternary.CStr());

    if (!QuaternaryItemDefinition)
        QuaternaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Quaternary).ToUtf8().c_str());

    if (QuaternaryItemDefinition)
    {
        FString QuaternaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)QuaternaryItemDefinition);
        Slot4.Add(TPair<FString, int>(QuaternaryFullPath, FConfiguration::QuaternaryAmount));
    }

    Slots.Add(Slot4);

    TArray<TPair<FString, int>> Slot5;

    auto QuinaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Quinary.CStr());

    if (!QuinaryItemDefinition)
        QuinaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Quinary).ToUtf8().c_str());

    if (QuinaryItemDefinition)
    {
        FString QuinaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)QuinaryItemDefinition);
        Slot5.Add(TPair<FString, int>(QuinaryFullPath, FConfiguration::QuinaryAmount));
    }

    Slots.Add(Slot5);

    TArray<TPair<FString, int>> Slot6;

    auto TrapsItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Traps.CStr());

    if (!TrapsItemDefinition)
        TrapsItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Traps).ToUtf8().c_str());

    if (TrapsItemDefinition)
    {
        FString TrapsFullPath = UKismetSystemLibrary::GetPathName((UObject*)TrapsItemDefinition);
        Slot6.Add(TPair<FString, int>(TrapsFullPath, FConfiguration::TrapsAmount));
    }

    Slots.Add(Slot6);

    std::uniform_int_distribution<int> Mats(186, 646);
    std::uniform_int_distribution<int> Gold(1200, 7500);

    TArray<TPair<FString, int>> Materials;
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/WoodItemData.WoodItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/StoneItemData.StoneItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/MetalItemData.MetalItemData"), Mats(rng)));

    if (VersionInfo.FortniteVersion >= 15)
    {
        Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/Athena_WadsItemData.Athena_WadsItemData"), Gold(rng)));
    }

    Slots.Add(Materials);

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 186);
    std::uniform_int_distribution<int> Shells(87, 576);
    std::uniform_int_distribution<int> Medium(124, 824);
    std::uniform_int_distribution<int> Light(186, 824);
    std::uniform_int_distribution<int> Rockets(3, 12);
    std::uniform_int_distribution<int> STW(186, 999);
    std::uniform_int_distribution<int> Arrows(12, 30);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells"), Shells(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium"), Medium(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight"), Light(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AmmoDataRockets.AmmoDataRockets"), 12));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataExplosive.AmmoDataExplosive"), STW(rng))); // make these and below optional???
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataEnergyCell.AmmoDataEnergyCell"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsHeavy.AmmoDataBulletsHeavy"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataShells.AmmoDataShells"), STW(rng)));

    if (VersionInfo.FortniteVersion >= 16.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Ammo/AthenaAmmoDataArrows.AthenaAmmoDataArrows"), Arrows(rng)));
    }

    Slots.Add(Ammo);

    return Slots;
}

void LateGame::EquipLoadout(AFortPlayerControllerAthena* PlayerController)
{
    if (!PlayerController || !PlayerController->HasAuthority())
        return;

    auto WorldInventory = PlayerController->WorldInventory;

    if (!WorldInventory)
        return;

    TArray<TArray<TPair<FString, int>>> Slots;
    bool bUsingVersionizedLoadout = false;

    if (IsOneShot())
    {
        Slots = LateGame::GetOSLoadout();
    }
    else if (FConfiguration::bUseCustomLoadout)
    {
        Slots = LateGame::GetCustomLoadout();
    }
    else if (FConfiguration::bUseVersionizedLoadout)
    {
        Slots = LateGame::GetVersionizedLoadout();
        bUsingVersionizedLoadout = true;
    }
    else
    {
        Slots = LateGame::GetLoadout();
    }

    if (!HasResolvableLateGamePrimaryItem(Slots))
    {
        if (bUsingVersionizedLoadout)
        {
            SDK::DbgLog(
                "[LateGame] no resolvable versionized primary item on FN %.2f; trying compatibility loadout\n",
                VersionInfo.FortniteVersion);
            Slots = LateGame::GetLoadout();
        }

        if (!HasResolvableLateGamePrimaryItem(Slots))
        {
            SDK::DbgLog(
                "[LateGame] selected loadout has no compatible primary item on FN %.2f; preserving inventory\n",
                VersionInfo.FortniteVersion);
            return;
        }
    }

    const int32 EntriesBeforeReset = WorldInventory->Inventory.ReplicatedEntries.Num();
    const int32 RemovedEntryCount = ClearInventoryForLateGameLoadout(WorldInventory);

    std::random_device rd;
    std::mt19937 rng(rd());

    for (int SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
    {
        const TArray<TPair<FString, int>>& Slot = Slots[SlotIndex];

        if (Slot.Num() == 0)
            continue;

        if (SlotIndex < 6)
        {
            std::vector<int32> ResolvableItemIndices;
            ResolvableItemIndices.reserve(Slot.Num());

            for (int32 CandidateIndex = 0;
                CandidateIndex < Slot.Num();
                ++CandidateIndex)
            {
                const auto& Candidate = Slot[CandidateIndex];
                if (FindObject<UFortWorldItemDefinition>(Candidate.Key().CStr()))
                {
                    ResolvableItemIndices.push_back(CandidateIndex);
                }
            }

            if (ResolvableItemIndices.empty())
            {
                printf("Slot %d has no loadable item definitions\n", SlotIndex + 1);
                continue;
            }

            std::uniform_int_distribution<int> dist(
                0, static_cast<int>(ResolvableItemIndices.size()) - 1);
            int RandomIndex = ResolvableItemIndices[dist(rng)];
            const TPair<FString, int>& RandomItem = Slot[RandomIndex];

            const FString& Path = RandomItem.Key();
            int Count = RandomItem.Value();

            std::string ConvertedPath = FStringToStdString(Path);
            printf("Slot %d: Trying to equip '%s' x%d\n", SlotIndex + 1, ConvertedPath.c_str(), Count);

            const UFortWorldItemDefinition* ItemDef = FindObject<UFortWorldItemDefinition>(Path.CStr());

            if (!ItemDef)
            {
                printf("Failed to find item: %s\n", ConvertedPath.c_str());
                continue;
            }

            int32 ClipSize = 0;
            int32 PhantomReserveAmmo = 0;

            if (auto WeaponDef = ItemDef->Cast<UFortWeaponItemDefinition>())
            {
                auto Stats = AFortInventory::GetStats(WeaponDef);

                if (Stats && Stats != (void*)-1)
                {
                    ClipSize = Stats->ClipSize;

                    if (WeaponDef->HasbUsesPhantomReserveAmmo() && WeaponDef->bUsesPhantomReserveAmmo)
                        PhantomReserveAmmo = (Stats->InitialClips - 1) * Stats->ClipSize;
                }
            }

            WorldInventory->GiveItem(ItemDef, Count, ClipSize, 0, true, true, PhantomReserveAmmo, {});
        }
        else
        {
            for (int i = 0; i < Slot.Num(); ++i)
            {
                const auto& Item = Slot[i];

                const FString& Path = Item.Key();
                int Count = Item.Value();

                std::string ConvertedPath = FStringToStdString(Path);
                printf("Slot %d: Trying to equip '%s' x%d\n", SlotIndex + 1, ConvertedPath.c_str(), Count);

                const UFortWorldItemDefinition* ItemDef = FindObject<UFortWorldItemDefinition>(Path.CStr());

                if (!ItemDef)
                {
                    printf("Failed to find item: %s\n", ConvertedPath.c_str());
                    continue;
                }

                int32 ClipSize = 0;
                int32 PhantomReserveAmmo = 0;

                if (auto WeaponDef = ItemDef->Cast<UFortWeaponItemDefinition>())
                {
                    auto Stats = AFortInventory::GetStats(WeaponDef);

                    if (Stats && Stats != (void*)-1)
                    {
                        ClipSize = Stats->ClipSize;

                        if (WeaponDef->HasbUsesPhantomReserveAmmo() && WeaponDef->bUsesPhantomReserveAmmo)
                            PhantomReserveAmmo = (Stats->InitialClips - 1) * Stats->ClipSize;
                    }
                }

                WorldInventory->GiveItem(ItemDef, Count, ClipSize, 0, true, true, PhantomReserveAmmo, {});
            }
        }
    }

    if (VersionInfo.FortniteVersion >= 19.0 && FConfiguration::bCrownSlomo)
    {
        auto CrownComponent = GetLateGameVictoryCrownComponent(PlayerController);
        auto CrownDefinition = ResolveLateGameVictoryCrownDefinition(CrownComponent);
        if (!CrownDefinition)
        {
            SDK::DbgLog("[LateGame] Victory Crown definition unavailable on %.2f\n",
                VersionInfo.FortniteVersion);
            return;
        }

        UFortWorldItem* CrownItem = nullptr;
        for (auto ItemInstance : WorldInventory->Inventory.ItemInstances)
        {
            if (IsUsableLateGameObject(ItemInstance) && ItemInstance->ItemEntry.ItemDefinition ==
                    CrownDefinition)
            {
                CrownItem = ItemInstance;
                break;
            }
        }

        bool bHasReplicatedCrownEntry = false;
        for (int32 Index = 0;
            Index < WorldInventory->Inventory.ReplicatedEntries.Num();
            Index++)
        {
            auto& Entry = WorldInventory->Inventory.ReplicatedEntries.Get(
                    Index, FFortItemEntry::Size());
            if (Entry.ItemDefinition == CrownDefinition)
            {
                bHasReplicatedCrownEntry = true;
                break;
            }
        }

        if (!CrownItem && bHasReplicatedCrownEntry)
        {
            WorldInventory->HandleInventoryLocalUpdate();
            for (auto ItemInstance : WorldInventory->Inventory.ItemInstances)
            {
                if (IsUsableLateGameObject(ItemInstance) &&
                    ItemInstance->ItemEntry.ItemDefinition == CrownDefinition)
                {
                    CrownItem = ItemInstance;
                    break;
                }
            }
        }

        bool bAlreadyHasCrown = CrownItem != nullptr || bHasReplicatedCrownEntry;
        if (!bAlreadyHasCrown)
        {
            TArray<FFortItemEntryStateValue> StateValues{};
            if (FFortItemEntryStateValue::StaticStruct() &&
                FFortItemEntryStateValue::HasIntValue() && FFortItemEntryStateValue::HasStateType())
            {
                const int32 StateValueSize = FFortItemEntryStateValue::Size();
                if (StateValueSize > 0 && StateValueSize <= 0x1000)
                {
                    auto StateValue = (FFortItemEntryStateValue*)malloc(StateValueSize);
                    if (StateValue)
                    {
                        memset((PBYTE)StateValue, 0, StateValueSize);
                        StateValue->IntValue = 1;
                        StateValue->StateType = 2;
                        StateValues.Add(*StateValue, StateValueSize);
                        free(StateValue);
                    }
                }
            }

            CrownItem = WorldInventory->GiveItem(CrownDefinition, 1, 0, 0, true, true, 0,
                StateValues);
            StateValues.Free();
        }

        UFortWorldItem* ComponentCrown = nullptr;
        if (CrownComponent)
        {
            if (auto GetCrownFunction = CrownComponent->GetFunction("GetCrownInPlayerInventory"))
            {
                ComponentCrown = CrownComponent->Call<UFortWorldItem*>(GetCrownFunction);
            }

            PlayerController->ForceNetUpdate();
        }

        SDK::DbgLog(
            "[LateGame] Victory Crown seed FN=%.2f controller=%p item=%p existing=%d replicatedEntry=%d componentCrown=%p\n",
            VersionInfo.FortniteVersion, (void*)PlayerController, (void*)CrownItem,
            bAlreadyHasCrown ? 1 : 0, bHasReplicatedCrownEntry ? 1 : 0, (void*)ComponentCrown);
    }

    SDK::DbgLog("[LateGame] loadout replaced controller=%p entries=%d removed=%d final=%d\n",
        (void*)PlayerController, EntriesBeforeReset, RemovedEntryCount,
        WorldInventory->Inventory.ReplicatedEntries.Num());
}
