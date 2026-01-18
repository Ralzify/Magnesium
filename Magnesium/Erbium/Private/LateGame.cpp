#include "pch.h"
#include "../Public/LateGame.h"
#include "../Public/Utils.h"
#include "../../FortniteGame/Public/FortInventory.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../Magnesium/Erbium/Public/Misc.h"

#include <random>
#include <chrono>

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

    if (VersionInfo.FortniteVersion >= 8.40)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_VR.WID_Assault_Infantry_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_SR.WID_Assault_Infantry_Athena_SR"), 1));
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

    if (VersionInfo.FortniteVersion >= 12.30)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Donut.WID_Pistol_Donut"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 14)
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
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_R_Ore_T03.WID_Shotgun_SemiAuto_Athena_R_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_VR_Ore_T03.WID_Shotgun_SemiAuto_Athena_VR_Ore_T03"), 1));

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
    else if (VersionInfo.FortniteVersion >= 29.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_UR.WID_Shotgun_Break_Cerberus_Athena_UR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_SR.WID_Shotgun_Break_Cerberus_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_VR.WID_Shotgun_Break_Cerberus_Athena_VR"), 1));
    }

    Slots.Add(Slot2);
    // Slot 2 End

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
    // Slot 3 End

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
    // Slot 4 End

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

    if (std::floor(VersionInfo.FortniteVersion) == 14)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Repulsors/CoreBR/WID_HighTower_Tomato_Repulsor_CoreBR.WID_HighTower_Tomato_Repulsor_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/RepulsorCannon/CoreBR/WID_HighTower_Tomato_RepulsorCannon_CoreBR.WID_HighTower_Tomato_RepulsorCannon_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Date/ChainLightning/CoreBR/WID_HighTower_Date_ChainLightning_CoreBR.WID_HighTower_Date_ChainLightning_CoreBR"), 1));
    }

    if (VersionInfo.FortniteVersion == 15.50 || VersionInfo.FortniteVersion == 17.30)
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

    if (VersionInfo.FortniteVersion >= 19.01)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/ShieldGenerator/WID_Athena_ShieldGenerator.WID_Athena_ShieldGenerator"), 2));
    }

    if (VersionInfo.FortniteVersion >= 21.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/GrappleGloves/Items/GrappleGloves/WID_GrappleGloves.WID_GrappleGloves"), 1));
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

    if (VersionInfo.FortniteVersion >= 24.20)
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
    // Slot 5 End

    // Slot 6 (Traps)
    TArray<TPair<FString, int>> Traps;
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Floor_Player_Launch_Pad_Athena.TID_Floor_Player_Launch_Pad_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_BouncePad_Athena.TID_Context_BouncePad_Athena"), 3));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_ContextTrap_Athena.TID_ContextTrap_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_Reinforced_Athena.TID_Context_Reinforced_Athena"), 2));

    Slots.Add(Traps);
    // Slot 6 End

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
    // Slot 7 End

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 576);
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

    if (VersionInfo.FortniteVersion >= 17.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/Ammo_Athena_Mother_Scooter.Ammo_Athena_Mother_Scooter"), 999));
    }

    Slots.Add(Ammo);
    // Slot 8 End

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
    // Slot 1 End

    // Slot 2 (No-Scope Sniper)
    TArray<TPair<FString, int>> Slot2;
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 3.10)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_UC_Ore_T03.WID_Sniper_NoScope_Athena_UC_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_R_Ore_T03.WID_Sniper_NoScope_Athena_R_Ore_T03"), 1));
    }

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
    // Slot 2 End

    // Slot 3 (Grappler)
    TArray<TPair<FString, int>> Slot3;

    if (VersionInfo.FortniteVersion >= 5.40)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Hook_Gun_VR_Ore_T03.WID_Hook_Gun_VR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion == 10.40)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Badger_Grape_VR.WID_Badger_Grape_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH"), 1));
    }

    Slots.Add(Slot3);
    // Slot 3 End

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
    // Slot 4 End

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
    // Slot 5 End

    // Slot 6 (Traps)
    TArray<TPair<FString, int>> Traps;
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Floor_Player_Launch_Pad_Athena.TID_Floor_Player_Launch_Pad_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_BouncePad_Athena.TID_Context_BouncePad_Athena"), 3));

    Slots.Add(Traps);
    // Slot 6 End

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
    // Slot 7 End

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 576);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));

    if (VersionInfo.FortniteVersion >= 17.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/Ammo_Athena_Mother_Scooter.Ammo_Athena_Mother_Scooter"), 999));
    }

    Slots.Add(Ammo);
    // Slot 8 End

    return Slots;
}

void LateGame::EquipLoadout(AFortPlayerControllerAthena* PlayerController)
{
    if (!PlayerController /* || !PlayerController->HasAuthority() */)
        return;

    auto WorldInventory = PlayerController->WorldInventory;

    if (!WorldInventory)
        return;

    auto Slots = IsOneShot() ? GetOSLoadout() : GetLoadout();

    std::random_device rd;
    std::mt19937 rng(rd());

    for (int SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
    {
        const TArray<TPair<FString, int>>& Slot = Slots[SlotIndex];

        if (Slot.Num() == 0)
            continue;

        if (SlotIndex < 6)
        {
            std::uniform_int_distribution<int> dist(0, Slot.Num() - 1);
            int RandomIndex = dist(rng);
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

            if (auto WeaponDef = ItemDef->Cast<UFortWeaponItemDefinition>())
            {
                auto Stats = AFortInventory::GetStats(WeaponDef);

                if (Stats && Stats != (void*)-1)
                {
                    ClipSize = Stats->ClipSize;
                }
            }

            auto AddResults = WorldInventory->GiveItem(ItemDef, Count, ClipSize);
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

                if (auto WeaponDef = ItemDef->Cast<UFortWeaponItemDefinition>())
                {
                    auto Stats = AFortInventory::GetStats(WeaponDef);

                    if (Stats && Stats != (void*)-1)
                    {
                        ClipSize = Stats->ClipSize;
                    }
                }

                WorldInventory->GiveItem(ItemDef, Count, ClipSize);
            }
        }
    }
}