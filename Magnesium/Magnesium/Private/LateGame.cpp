#include "pch.h"
#include "../Public/LateGame.h"
#include "../Public/Utils.h"
#include "../../FortniteGame/Public/FortInventory.h"

#include <random>
#include <chrono>

FLateGameItem LateGame::GetShotgun()
{
    static std::mt19937 gen(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    static UEAllocatedVector<FLateGameItem> Shotguns
    {
    };

    if (Shotguns.size() == 0)
    {
        if (VersionInfo.FortniteVersion >= 3.31)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_SR.WID_Shotgun_SlugFire_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_VR.WID_Shotgun_SlugFire_Athena_VR")));
        }
        else if (VersionInfo.FortniteVersion >= 6.31)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_SR_Ore_T03.WID_Shotgun_Standard_Athena_SR_Ore_T03")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_VR_Ore_T03.WID_Shotgun_Standard_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 9.00)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_SR_Ore_T03.WID_Shotgun_Combat_Athena_SR_Ore_T03")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_VR_Ore_T03.WID_Shotgun_Combat_Athena_VR_Ore_T03")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_R_Ore_T03.WID_Shotgun_Combat_Athena_R_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 9.40)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 13.00)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Charge_Athena_SR_Ore_T03.WID_Shotgun_Charge_Athena_SR_Ore_T03")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Charge_Athena_VR_Ore_T03.WID_Shotgun_Charge_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 19.00)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_SR.WID_Shotgun_CoreBurst_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_VR.WID_Shotgun_CoreBurst_Athena_VR")));
        }
        else if (VersionInfo.FortniteVersion >= 20.20)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_SR_Ore_T03.WID_Shotgun_Break_Action_Athena_SR_Ore_T03")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_VR_Ore_T03.WID_Shotgun_Break_Action_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 21.30)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_SR.WID_Shotgun_OverLoad_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_VR.WID_Shotgun_OverLoad_Athena_VR")));
        }
        else if (VersionInfo.FortniteVersion >= 23.00)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_SR.WID_Shotgun_MusterPump_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_VR.WID_Shotgun_MusterPump_Athena_VR")));
        }
        else if (VersionInfo.FortniteVersion >= 24.00)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_UR.WID_Shotgun_RadicalPump_Athena_UR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_SR.WID_Shotgun_RadicalPump_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_VR.WID_Shotgun_RadicalPump_Athena_VR")));
        }
        else if (VersionInfo.FortniteVersion >= 25.11)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_SR.WID_Shotgun_Chrono_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_VR.WID_Shotgun_Chrono_Athena_VR")));
        }
        else if (VersionInfo.FortniteVersion >= 28.00)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_UR_Boss.WID_Shotgun_Pump_Paprika_Athena_UR_Boss")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_SR.WID_Shotgun_Pump_Paprika_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_VR.WID_Shotgun_Pump_Paprika_Athena_VR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_UR_Boss.WID_Shotgun_Auto_Paprika_Athena_UR_Boss")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_SR.WID_Shotgun_Auto_Paprika_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_VR.WID_Shotgun_Auto_Paprika_Athena_VR")));
        }
        else if (VersionInfo.FortniteVersion >= 29.00)
        {
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_UR.WID_Shotgun_Break_Cerberus_Athena_UR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_SR.WID_Shotgun_Break_Cerberus_Athena_SR")));
            Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_VR.WID_Shotgun_Break_Cerberus_Athena_VR")));
        }

        Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_UC_Ore_T03.WID_Shotgun_Standard_Athena_UC_Ore_T03")));
        Shotguns.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_C_Ore_T03.WID_Shotgun_Standard_Athena_C_Ore_T03")));
    }

    if (Shotguns.size() > 0)
    {
        std::uniform_int_distribution<size_t> dis(0, Shotguns.size() - 1);
        return Shotguns[dis(gen)];
    }

    return FLateGameItem();
}

FLateGameItem LateGame::GetAssaultRifle()
{
    static std::mt19937 gen(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    static UEAllocatedVector<FLateGameItem> AssaultRifles {};

    if (AssaultRifles.size() == 0)
    {
        if (VersionInfo.FortniteVersion > 22.40 && VersionInfo.FortniteVersion <= 26.30)
        {
            if (VersionInfo.FortniteVersion < 25.00)
            {
                AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_SR.WID_Assault_MusterScoped_Athena_SR")));
                AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_VR.WID_Assault_MusterScoped_Athena_VR")));
            }
            else if (VersionInfo.FortniteVersion >= 26.00)
            {
                AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_SR.WID_Assault_FlipMag_Athena_SR")));
                AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_VR.WID_Assault_FlipMag_Athena_VR")));
            }
            else if (VersionInfo.FortniteVersion >= 25.00)
            {
                AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_SR.WID_Assault_Chrono_Pan_Rifle_Athena_SR")));
                AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_VR.WID_Assault_Chrono_Pan_Rifle_Athena_VR")));
            }

            AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_SR_Ore_T03.WID_Assault_AutoHigh_Athena_SR_Ore_T03")));
            AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_VR_Ore_T03.WID_Assault_AutoHigh_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 19.00 && VersionInfo.FortniteVersion <= 22.40)
        {
            if (VersionInfo.FortniteVersion < 21 && VersionInfo.FortniteVersion != 20.00)
            {
                AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_RedDotAR_Athena_SR.WID_Assault_RedDotAR_Athena_SR")));
                AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_RedDotAR_Athena_VR.WID_Assault_RedDotAR_Athena_VR")));
            }

            AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_SR.WID_Assault_CoreAR_Athena_SR")));
            AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_VR.WID_Assault_CoreAR_Athena_VR")));
        }
        else
        {
            AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_SR_Ore_T03.WID_Assault_AutoHigh_Athena_SR_Ore_T03")));
            AssaultRifles.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_VR_Ore_T03.WID_Assault_AutoHigh_Athena_VR_Ore_T03")));
        }
    }

    if (AssaultRifles.size() > 0)
    {
        std::uniform_int_distribution<size_t> dis(0, AssaultRifles.size() - 1);
        return AssaultRifles[dis(gen)];
    }

    return FLateGameItem();
}


FLateGameItem LateGame::GetSniper()
{
    static std::mt19937 gen(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    static UEAllocatedVector<FLateGameItem> Snipers {};

    if (Snipers.size() == 0)
    {
        if (VersionInfo.FortniteVersion >= 3.10)
        {
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_R_Ore_T03.WID_Sniper_NoScope_Athena_R_Ore_T03")));
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_UC_Ore_T03.WID_Sniper_NoScope_Athena_UC_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 5.21)
        {
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_SR_Ore_T03.WID_Sniper_Heavy_Athena_SR_Ore_T03")));
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_VR_Ore_T03.WID_Sniper_Heavy_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 7.10)
        {
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03")));
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 13.00)
        {
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03")));
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 19.00)
        {
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_SR.WID_Sniper_CoreSniper_Athena_SR")));
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_VR.WID_Sniper_CoreSniper_Athena_VR")));
        }
        else
        {
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Athena_SR_Ore_T03")));
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Athena_VR_Ore_T03.WID_Sniper_BoltAction_Athena_VR_Ore_T03")));
            Snipers.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Athena_R_Ore_T03.WID_Sniper_BoltAction_Athena_R_Ore_T03")));
		}
    }

    if (Snipers.size() > 0)
    {
        std::uniform_int_distribution<size_t> dis(0, Snipers.size() - 1);
        return Snipers[dis(gen)];
    }

    return FLateGameItem();
}

FLateGameItem LateGame::GetHeal()
{
    static std::mt19937 gen(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    static UEAllocatedVector<FLateGameItem> Heals {};

    if (Heals.size() == 0)
    {
        if (VersionInfo.FortniteVersion >= 2.3)
        {
            Heals.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/SuperMedkit/Athena_SuperMedkit.Athena_SuperMedkit")));
        }
        else if (VersionInfo.FortniteVersion >= 9.30)
        {
            Heals.push_back(FLateGameItem(6, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/ChillBronco/Athena_ChillBronco.Athena_ChillBronco")));
        }
        else if (VersionInfo.FortniteVersion >= 11.00)
        {
            Heals.push_back(FLateGameItem(4, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/Flopper/WID_Athena_Flopper.WID_Athena_Flopper")));
            Heals.push_back(FLateGameItem(3, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/Flopper/Effective/WID_Athena_Flopper_Effective.WID_Athena_Flopper_Effective")));
        }
        else
        {
            Heals.push_back(FLateGameItem(6, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/ShieldSmall/Athena_ShieldSmall.Athena_ShieldSmall")));
            Heals.push_back(FLateGameItem(2, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/Shields/Athena_Shields.Athena_Shields")));
            Heals.push_back(FLateGameItem(3, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/Medkit/Athena_Medkit.Athena_Medkit")));
            Heals.push_back(FLateGameItem(2, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff"))); // 1.8, might wanna move to an if statement but idk how good lategame is on 1.7.2
        }
    }

    if (Heals.size() > 0)
    {
        std::uniform_int_distribution<size_t> dis(0, Heals.size() - 1);
        return Heals[dis(gen)];
    }

    return FLateGameItem();
}

FLateGameItem LateGame::GetConsumable()
{
    static std::mt19937 gen(static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

    static UEAllocatedVector<FLateGameItem> Consumables{};

    if (Consumables.size() == 0)
    {
        if (VersionInfo.FortniteVersion >= 3.4 && VersionInfo.FortniteVersion <= 7.10)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_SR_T03.WID_RC_Rocket_Athena_SR_T03")));
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_VR_T03.WID_RC_Rocket_Athena_VR_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 3.5)
        {
            Consumables.push_back(FLateGameItem(2, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/TowerGrenade/Athena_TowerGrenade.Athena_TowerGrenade")));
        }
        else if (VersionInfo.FortniteVersion >= 5.30)
        {
            Consumables.push_back(FLateGameItem(2, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item")));
            Consumables.push_back(FLateGameItem(6, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade")));
        }
        else if (VersionInfo.FortniteVersion >= 5.40)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Hook_Gun_VR_Ore_T03.WID_Hook_Gun_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 5.41)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/SuperTowerGrenade/Levels/PortAFort_A/Athena_SuperTowerGrenade_A.Athena_SuperTowerGrenade_A")));
        }
        else if (VersionInfo.FortniteVersion >= 6.02)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_SR_Ore_T03.WID_Launcher_Military_Athena_SR_Ore_T03")));
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_VR_Ore_T03.WID_Launcher_Military_Athena_VR_Ore_T03")));
        }
        else if (VersionInfo.FortniteVersion >= 7.30)
        {
            Consumables.push_back(FLateGameItem(6, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade")));
        }
        else if (VersionInfo.FortniteVersion >= 8.11)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_UC.WID_Pistol_Flintlock_Athena_UC")));
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_C.WID_Pistol_Flintlock_Athena_C")));
        }
        else if (VersionInfo.FortniteVersion >= 10.20)
        {
            Consumables.push_back(FLateGameItem(2, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2")));
        }
        else if (VersionInfo.FortniteVersion >= 11.00)
        {
            Consumables.push_back(FLateGameItem(4, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/Bucket/WID_Athena_Bucket_Old.WID_Athena_Bucket_Old")));
        }
        else if (VersionInfo.FortniteVersion >= 12.00)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH")));
            Consumables.push_back(FLateGameItem(3, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/DangerGrape/WID_Athena_DangerGrape.WID_Athena_DangerGrape")));
        }
        else if (VersionInfo.FortniteVersion >= 12.30)
        {
            Consumables.push_back(FLateGameItem(6, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun")));
        }
        else if (VersionInfo.FortniteVersion >= 13.00)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/Boss/WID_Boss_GrapplingHoot.WID_Boss_GrapplingHoot")));
        }
        else if (std::floor(VersionInfo.FortniteVersion) == 14)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/HighTower/Items/Tomato/Repulsors/CoreBR/WID_HighTower_Tomato_Repulsor_CoreBR.WID_HighTower_Tomato_Repulsor_CoreBR")));
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/HighTower/Items/Tomato/RepulsorCannon/CoreBR/WID_HighTower_Tomato_RepulsorCannon_CoreBR.WID_HighTower_Tomato_RepulsorCannon_CoreBR")));
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/HighTower/Items/Date/ChainLightning/CoreBR/WID_HighTower_Date_ChainLightning_CoreBR.WID_HighTower_Date_ChainLightning_CoreBR")));
            // idk what ones work on erbium but
        }
        else if (VersionInfo.FortniteVersion == 15.50 || VersionInfo.FortniteVersion == 17.30)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/LTM/Builder/Gameplay/Yeetknock_pistol/Builder_WID_YEETknock_UR.Builder_WID_YEETknock_UR")));
        }
        else if (VersionInfo.FortniteVersion >= 18.00)
        {
            Consumables.push_back(FLateGameItem(1, FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Launcher_Shockwave_Athena_SR.WID_Launcher_Shockwave_Athena_SR")));
        }
        else if (VersionInfo.FortniteVersion >= 19.01)
        {
            Consumables.push_back(FLateGameItem(2, FindObject<UFortItemDefinition>(L"/FlipperGameplay/Items/ShieldGenerator/WID_Athena_ShieldGenerator.WID_Athena_ShieldGenerator")));
        }
    }

    if (Consumables.size() > 0)
    {
        std::uniform_int_distribution<size_t> dis(0, Consumables.size() - 1);
        return Consumables[dis(gen)];
    }

    return FLateGameItem();
}

const UFortItemDefinition* LateGame::GetAmmo(EAmmoType AmmoType)
{
    static UEAllocatedVector<const UFortItemDefinition*> Ammos
    {
        FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight"),
        FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells"),
        FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium"),
        FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AmmoDataRockets.AmmoDataRockets"),
        FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy")
    };

    return Ammos[(uint8)AmmoType];
}

const UFortItemDefinition* LateGame::GetResource(EFortResourceType ResourceType)
{
    static UEAllocatedVector<const UFortItemDefinition*> Resources
    {
        FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData"),
        FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData"),
        FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData")
    };

    return Resources[(uint8)ResourceType];
}
